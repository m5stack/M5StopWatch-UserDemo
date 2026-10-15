/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "config_ap.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <dns_server.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <lwip/ip_addr.h>
#include <nvs_flash.h>

namespace badge::config_ap {
namespace {

constexpr const char* _tag                   = "Badge-ConfigAp";
constexpr const char* _ap_ssid_prefix        = "M5StopWatch";
constexpr const char* _ap_url                = "http://192.168.4.1";
constexpr EventBits_t _exit_requested_bit    = BIT0;
constexpr std::size_t _max_upload_size_bytes = 2 * 1024 * 1024;
constexpr const char* _json_content_type     = "application/json";

extern const char badge_config_ap_html_start[] asm("_binary_badge_config_ap_html_start");
extern const char badge_config_ap_html_end[] asm("_binary_badge_config_ap_html_end");

constexpr const char* _captive_portal_urls[] = {
    "/hotspot-detect.html",       "/generate_204*", "/mobile/status.php",
    "/check_network_status.txt",  "/ncsi.txt",      "/fwlink/",
    "/connectivity-check.html",   "/success.txt",   "/portal.html",
    "/library/test/success.html",
};

bool ensure_wifi_stack_ready()
{
    static std::mutex mutex;
    static bool initialized = false;

    std::lock_guard<std::mutex> lock(mutex);
    if (initialized) {
        return true;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(_tag, "nvs init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(_tag, "netif init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(_tag, "event loop init failed: %s", esp_err_to_name(ret));
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable         = false;
    ret                    = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(_tag, "wifi init failed: %s", esp_err_to_name(ret));
        return false;
    }

    initialized = true;
    return true;
}

std::string make_ap_ssid()
{
    uint8_t mac[6] = {};
    esp_err_t ret  = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (ret != ESP_OK) {
        ESP_LOGW(_tag, "read mac failed: %s", esp_err_to_name(ret));
        return std::string(_ap_ssid_prefix);
    }

    char ssid[32] = {};
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", _ap_ssid_prefix, mac[4], mac[5]);
    return std::string(ssid);
}

class Session {
public:
    Session(const std::function<void(std::string_view)>& onLog, const Callbacks& callbacks)
        : _on_log(onLog), _callbacks(callbacks)
    {
    }

    bool run()
    {
        if (!ensure_wifi_stack_ready()) {
            log("Wi-Fi initialization failed");
            return false;
        }

        _event_group = xEventGroupCreate();
        if (_event_group == nullptr) {
            log("Failed to create sync event group");
            return false;
        }

        if (!start_access_point() || !start_web_server()) {
            stop();
            return false;
        }

        log("Connect to Wi-Fi: " + _ssid + "\nAnd open page:\n" + std::string(_ap_url));

        xEventGroupWaitBits(_event_group, _exit_requested_bit, pdTRUE, pdFALSE, portMAX_DELAY);

        stop();

        log("Exited edit mode");
        return true;
    }

private:
    void log(const std::string& message) const
    {
        ESP_LOGI(_tag, "%s", message.c_str());
        if (_on_log) {
            _on_log(message);
        }
    }

    bool start_access_point()
    {
        if (_ap_netif == nullptr) {
            _ap_netif = esp_netif_create_default_wifi_ap();
        }
        if (_ap_netif == nullptr) {
            log("Failed to create AP network interface");
            return false;
        }

        esp_netif_ip_info_t ip_info;
        IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
        IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(_ap_netif);
        esp_netif_set_ip_info(_ap_netif, &ip_info);
        esp_netif_dhcps_start(_ap_netif);

        _dns_server = std::make_unique<DnsServer>();
        _dns_server->Start(ip_info.gw);

        _ssid = make_ap_ssid();

        wifi_config_t wifi_config = {};
        strncpy(reinterpret_cast<char*>(wifi_config.ap.ssid), _ssid.c_str(), sizeof(wifi_config.ap.ssid) - 1);
        wifi_config.ap.ssid_len       = _ssid.size();
        wifi_config.ap.max_connection = 4;
        wifi_config.ap.authmode       = WIFI_AUTH_OPEN;

        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED && ret != ESP_ERR_WIFI_MODE) {
            ESP_LOGW(_tag, "wifi stop before ap failed: %s", esp_err_to_name(ret));
        }

        ret = esp_wifi_set_mode(WIFI_MODE_AP);
        if (ret != ESP_OK) {
            log("Failed to set AP mode");
            return false;
        }

        ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
        if (ret != ESP_OK) {
            log("Failed to apply AP configuration");
            return false;
        }

        ret = esp_wifi_set_ps(WIFI_PS_NONE);
        if (ret != ESP_OK) {
            log("Failed to disable Wi-Fi power save");
            return false;
        }

        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            log("Failed to start AP");
            return false;
        }

        return true;
    }

    bool start_web_server()
    {
        httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
        config.max_uri_handlers  = 24;
        config.recv_wait_timeout = 15;
        config.send_wait_timeout = 15;
        config.uri_match_fn      = httpd_uri_match_wildcard;

        esp_err_t ret = httpd_start(&_server, &config);
        if (ret != ESP_OK) {
            log("Failed to start web server");
            return false;
        }

        httpd_uri_t index = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = &Session::handle_index,
            .user_ctx = this,
        };
        httpd_uri_t upload = {
            .uri      = "/upload",
            .method   = HTTP_POST,
            .handler  = &Session::handle_upload,
            .user_ctx = this,
        };
        httpd_uri_t state = {
            .uri      = "/badge/state",
            .method   = HTTP_GET,
            .handler  = &Session::handle_state,
            .user_ctx = this,
        };
        httpd_uri_t image = {
            .uri      = "/badge/image",
            .method   = HTTP_GET,
            .handler  = &Session::handle_image,
            .user_ctx = this,
        };
        httpd_uri_t active = {
            .uri      = "/badge/active",
            .method   = HTTP_POST,
            .handler  = &Session::handle_set_active,
            .user_ctx = this,
        };
        httpd_uri_t remove = {
            .uri      = "/badge/image",
            .method   = HTTP_DELETE,
            .handler  = &Session::handle_delete_image,
            .user_ctx = this,
        };
        httpd_uri_t close = {
            .uri      = "/close",
            .method   = HTTP_POST,
            .handler  = &Session::handle_close,
            .user_ctx = this,
        };

        httpd_uri_t captive = {
            .uri      = nullptr,
            .method   = HTTP_GET,
            .handler  = &Session::handle_captive_portal,
            .user_ctx = this,
        };

        ret = httpd_register_uri_handler(_server, &index);
        if (ret == ESP_OK) {
            ret = httpd_register_uri_handler(_server, &state);
        }
        if (ret == ESP_OK) {
            ret = httpd_register_uri_handler(_server, &image);
        }
        if (ret == ESP_OK) {
            ret = httpd_register_uri_handler(_server, &active);
        }
        if (ret == ESP_OK) {
            ret = httpd_register_uri_handler(_server, &remove);
        }
        if (ret == ESP_OK) {
            ret = httpd_register_uri_handler(_server, &upload);
        }
        if (ret == ESP_OK) {
            ret = httpd_register_uri_handler(_server, &close);
        }
        if (ret == ESP_OK) {
            for (const auto* url : _captive_portal_urls) {
                captive.uri = url;
                ret         = httpd_register_uri_handler(_server, &captive);
                if (ret != ESP_OK) {
                    break;
                }
            }
        }
        if (ret != ESP_OK) {
            log("Failed to register web routes");
            return false;
        }

        return true;
    }

    void stop()
    {
        if (_server != nullptr) {
            httpd_stop(_server);
            _server = nullptr;
        }

        if (_dns_server) {
            _dns_server->Stop();
            _dns_server.reset();
        }

        esp_err_t ret = esp_wifi_stop();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED && ret != ESP_ERR_WIFI_MODE) {
            ESP_LOGW(_tag, "wifi stop failed: %s", esp_err_to_name(ret));
        }

        if (_ap_netif != nullptr) {
            esp_netif_destroy_default_wifi(_ap_netif);
            _ap_netif = nullptr;
        }

        if (_event_group != nullptr) {
            vEventGroupDelete(_event_group);
            _event_group = nullptr;
        }
    }

    static Session* self_from_request(httpd_req_t* req)
    {
        return static_cast<Session*>(req->user_ctx);
    }

    static bool parse_slot_from_query(httpd_req_t* req, std::size_t& slot)
    {
        char query[64] = {};
        if (httpd_req_get_url_query_len(req) <= 0 || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
            return false;
        }

        char slot_value[16] = {};
        if (httpd_query_key_value(query, "slot", slot_value, sizeof(slot_value)) != ESP_OK) {
            return false;
        }

        slot = static_cast<std::size_t>(strtoul(slot_value, nullptr, 10));
        return true;
    }

    static void send_json(httpd_req_t* req, const std::string& body)
    {
        httpd_resp_set_type(req, _json_content_type);
        httpd_resp_send(req, body.c_str(), body.size());
    }

    static std::string make_state_json(const BadgeState& state, std::string_view apSsid)
    {
        std::string json = "{";
        json += "\"apSsid\":\"" + std::string(apSsid) + "\",";
        json += "\"apUrl\":\"" + std::string(_ap_url) + "\",";
        json += "\"slotCount\":" + std::to_string(state.slotCount) + ",";
        json += "\"activeSlot\":" + std::to_string(state.activeSlot) + ",";
        json += "\"slots\":[";
        for (std::size_t index = 0; index < state.slots.size(); ++index) {
            const auto& slot = state.slots[index];
            if (index != 0) {
                json += ",";
            }
            json += "{";
            json += "\"slot\":" + std::to_string(slot.slot) + ",";
            json += "\"hasImage\":" + std::string(slot.hasImage ? "true" : "false") + ",";
            json += "\"isActive\":" + std::string(slot.isActive ? "true" : "false") + ",";
            json += "\"imageUrl\":\"/badge/image?slot=" + std::to_string(slot.slot) + "\"";
            json += "}";
        }
        json += "]}";
        return json;
    }

    static std::string make_message_json(std::string_view status, std::string_view message)
    {
        return std::string("{\"status\":\"") + std::string(status) + "\",\"message\":\"" + std::string(message) + "\"}";
    }

    static esp_err_t handle_index(httpd_req_t* req)
    {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_send(req, badge_config_ap_html_start, badge_config_ap_html_end - badge_config_ap_html_start);
        return ESP_OK;
    }

    static esp_err_t handle_state(httpd_req_t* req)
    {
        auto* self = self_from_request(req);
        if (self == nullptr || self->_callbacks.onGetState == nullptr) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "state handler missing");
            return ESP_FAIL;
        }

        send_json(req, make_state_json(self->_callbacks.onGetState(), self->_ssid));
        return ESP_OK;
    }

    static esp_err_t handle_image(httpd_req_t* req)
    {
        auto* self = self_from_request(req);
        if (self == nullptr || self->_callbacks.onGetImage == nullptr) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image handler missing");
            return ESP_FAIL;
        }

        std::size_t slot = 0;
        if (!parse_slot_from_query(req, slot)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot");
            return ESP_FAIL;
        }

        ImageData image;
        std::string message;
        if (!self->_callbacks.onGetImage(slot, image, message)) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, message.empty() ? "image not found" : message.c_str());
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, image.contentType.c_str());
        httpd_resp_send(req, reinterpret_cast<const char*>(image.data.data()), image.data.size());
        return ESP_OK;
    }

    static esp_err_t handle_set_active(httpd_req_t* req)
    {
        auto* self = self_from_request(req);
        if (self == nullptr || self->_callbacks.onSetActive == nullptr) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set active handler missing");
            return ESP_FAIL;
        }

        std::size_t slot = 0;
        if (!parse_slot_from_query(req, slot)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot");
            return ESP_FAIL;
        }

        std::string message;
        if (!self->_callbacks.onSetActive(slot, message)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message.empty() ? "set active failed" : message.c_str());
            return ESP_FAIL;
        }

        send_json(req, make_message_json("ok", message));
        return ESP_OK;
    }

    static esp_err_t handle_delete_image(httpd_req_t* req)
    {
        auto* self = self_from_request(req);
        if (self == nullptr || self->_callbacks.onDelete == nullptr) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete handler missing");
            return ESP_FAIL;
        }

        std::size_t slot = 0;
        if (!parse_slot_from_query(req, slot)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot");
            return ESP_FAIL;
        }

        std::string message;
        if (!self->_callbacks.onDelete(slot, message)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message.empty() ? "delete failed" : message.c_str());
            return ESP_FAIL;
        }

        send_json(req, make_message_json("ok", message));
        return ESP_OK;
    }

    static esp_err_t handle_captive_portal(httpd_req_t* req)
    {
        auto* self      = self_from_request(req);
        std::string url = std::string(_ap_url) + "/?_=" + std::to_string(esp_timer_get_time());
        if (self != nullptr) {
            self->log("Received captive portal probe, redirecting to upload page");
        }

        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", url.c_str());
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

    static esp_err_t handle_close(httpd_req_t* req)
    {
        auto* self = self_from_request(req);
        xEventGroupSetBits(self->_event_group, _exit_requested_bit);
        httpd_resp_sendstr(req, "closing");
        return ESP_OK;
    }

    static esp_err_t handle_upload(httpd_req_t* req)
    {
        auto* self = self_from_request(req);
        if (self == nullptr || self->_callbacks.onUpload == nullptr) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload handler missing");
            return ESP_FAIL;
        }

        if (req->content_len <= 0 || static_cast<std::size_t>(req->content_len) > _max_upload_size_bytes) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid upload size");
            return ESP_FAIL;
        }

        UploadRequest upload_request;

        if (!parse_slot_from_query(req, upload_request.slot)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot");
            return ESP_FAIL;
        }

        int header_length = httpd_req_get_hdr_value_len(req, "X-File-Name");
        if (header_length > 0) {
            std::string encoded_name(static_cast<std::size_t>(header_length) + 1, '\0');
            if (httpd_req_get_hdr_value_str(req, "X-File-Name", encoded_name.data(), header_length + 1) == ESP_OK) {
                encoded_name.resize(static_cast<std::size_t>(header_length));
                upload_request.fileName = encoded_name;
            }
        }
        if (upload_request.fileName.empty()) {
            upload_request.fileName = "badge.jpg";
        }

        char content_type[64] = {};
        if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK) {
            upload_request.contentType = content_type;
        }

        upload_request.data.resize(static_cast<std::size_t>(req->content_len));
        std::size_t offset = 0;
        while (offset < upload_request.data.size()) {
            const int received = httpd_req_recv(req, reinterpret_cast<char*>(upload_request.data.data() + offset),
                                                upload_request.data.size() - offset);
            if (received <= 0) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to read upload body");
                return ESP_FAIL;
            }
            offset += static_cast<std::size_t>(received);
        }

        std::string message;
        if (!self->_callbacks.onUpload(upload_request, message)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message.empty() ? "save failed" : message.c_str());
            return ESP_FAIL;
        }

        send_json(req, make_message_json("ok", message.empty() ? "upload success" : message));
        return ESP_OK;
    }

    httpd_handle_t _server          = nullptr;
    EventGroupHandle_t _event_group = nullptr;
    std::function<void(std::string_view)> _on_log;
    Callbacks _callbacks;
    std::string _ssid;
    std::unique_ptr<DnsServer> _dns_server;
    esp_netif_t* _ap_netif = nullptr;
};

}  // namespace

bool run(const std::function<void(std::string_view)>& onLog, const Callbacks& callbacks)
{
    return Session(onLog, callbacks).run();
}

}  // namespace badge::config_ap
