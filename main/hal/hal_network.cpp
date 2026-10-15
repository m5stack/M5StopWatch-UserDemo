/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "utils/settings/settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <mooncake_log.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#include <mutex>
#include <string>

namespace {

constexpr const char* _tag                     = "HAL-WiFi";
constexpr const char* _settings_namespace      = "system";
constexpr const char* _wifi_enabled_key        = "wifi_enabled";
constexpr const char* _wifi_ap_ssid_prefix     = "M5StopWatch";
constexpr const char* _wifi_config_language    = "en-US";
constexpr uint32_t _station_connect_timeout_ms = 30000;

uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

bool deadline_expired(uint32_t now, uint32_t deadline)
{
    return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}

class NetworkController {
public:
    void init()
    {
        const bool enabled = load_enabled_from_settings();
        {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            _status.enabled = enabled;
            _wifi_enabled   = enabled;
        }

        if (enabled) {
            start_station_or_config_ap("Wi-Fi enabled");
        } else {
            set_off_status("Wi-Fi off");
        }
    }

    void tick()
    {
        bool should_fallback_to_ap = false;
        bool should_restart_sta    = false;
        {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            const uint32_t now = millis();
            should_fallback_to_ap =
                _wifi_enabled && !_badge_active && _status.mode == Hal::WifiMode::Connecting &&
                !_status.connected && deadline_expired(now, _station_deadline_ms);
            should_restart_sta = _restart_station_requested && _wifi_enabled && !_badge_active;
            if (should_restart_sta) {
                _restart_station_requested = false;
            }
        }

        if (should_fallback_to_ap) {
            mclog::tagWarn(_tag, "station connect timeout, entering config AP");
            start_config_ap("Connection timed out");
            return;
        }

        if (should_restart_sta) {
            start_station_or_config_ap("Configuration saved");
        }
    }

    void set_enabled(bool enabled, bool save_to_settings)
    {
        if (save_to_settings) {
            Settings settings(_settings_namespace, true);
            settings.SetBool(_wifi_enabled_key, enabled);
        }

        {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            _wifi_enabled              = enabled;
            _status.enabled            = enabled;
            _restart_station_requested = false;
        }

        if (enabled) {
            start_station_or_config_ap("Wi-Fi enabled");
        } else {
            stop_wifi_owner();
            set_off_status("Wi-Fi off");
        }
    }

    bool is_enabled(bool load_from_settings)
    {
        if (load_from_settings) {
            return load_enabled_from_settings();
        }

        std::lock_guard<std::recursive_mutex> lock(_mutex);
        return _wifi_enabled;
    }

    Hal::WifiStatus status()
    {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        if (_initialized && !_badge_active) {
            auto& wifi = WifiManager::GetInstance();
            if (_status.mode == Hal::WifiMode::Connected || _status.mode == Hal::WifiMode::Connecting) {
                _status.connected = wifi.IsConnected();
                _status.ssid      = wifi.GetSsid();
                _status.ipAddress = wifi.GetIpAddress();
                if (_status.connected) {
                    _status.mode    = Hal::WifiMode::Connected;
                    _status.message = "Connected";
                }
            } else if (_status.mode == Hal::WifiMode::ConfigAp) {
                _status.configMode = wifi.IsConfigMode();
                _status.apSsid     = wifi.GetApSsid();
                _status.apUrl      = wifi.GetApWebUrl();
            }
        }
        return _status;
    }

    void enter_badge_ap()
    {
        {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            _badge_active              = true;
            _restart_station_requested = false;
        }

        ensure_initialized();
        stop_wifi_owner();

        std::lock_guard<std::recursive_mutex> lock(_mutex);
        _station_deadline_ms = 0;
        _status.mode         = Hal::WifiMode::BadgeAp;
        _status.badgeMode    = true;
        _status.connected    = false;
        _status.configMode   = false;
        _status.ssid.clear();
        _status.ipAddress.clear();
        _status.apSsid.clear();
        _status.apUrl        = "http://192.168.4.1";
        _status.message      = "Badge edit AP";
    }

    void exit_badge_ap()
    {
        bool should_resume_wifi = false;
        {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            _badge_active       = false;
            _status.badgeMode   = false;
            should_resume_wifi  = _wifi_enabled;
        }

        if (should_resume_wifi) {
            start_station_or_config_ap("Resuming Wi-Fi");
        } else {
            set_off_status("Wi-Fi off");
        }
    }

private:
    bool load_enabled_from_settings()
    {
        Settings settings(_settings_namespace, false);
        return settings.GetBool(_wifi_enabled_key, false);
    }

    bool has_saved_credentials()
    {
        return !SsidManager::GetInstance().GetSsidList().empty();
    }

    bool ensure_initialized()
    {
        {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            if (_initialized) {
                return true;
            }
        }

        WifiManagerConfig config;
        config.ssid_prefix                       = _wifi_ap_ssid_prefix;
        config.language                          = _wifi_config_language;
        config.station_scan_min_interval_seconds = 5;
        config.station_scan_max_interval_seconds = 30;

        auto& wifi = WifiManager::GetInstance();
        wifi.SetEventCallback([](WifiEvent event, const std::string& data) {
            controller().handle_event(event, data);
        });

        if (!wifi.Initialize(config)) {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            _status.mode    = Hal::WifiMode::Error;
            _status.message = "Wi-Fi initialization failed";
            return false;
        }

        std::lock_guard<std::recursive_mutex> lock(_mutex);
        _initialized = true;
        return true;
    }

    void start_station_or_config_ap(const std::string& message)
    {
        if (!ensure_initialized()) {
            return;
        }

        if (!has_saved_credentials()) {
            start_config_ap("No saved Wi-Fi");
            return;
        }

        {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            if (_badge_active || !_wifi_enabled) {
                return;
            }
        }

        auto& wifi = WifiManager::GetInstance();
        if (wifi.IsConfigMode()) {
            wifi.StopConfigAp();
        }
        wifi.StartStation();

        std::lock_guard<std::recursive_mutex> lock(_mutex);
        _station_deadline_ms = millis() + _station_connect_timeout_ms;
        _status.enabled      = true;
        _status.mode         = Hal::WifiMode::Connecting;
        _status.connected    = false;
        _status.configMode   = false;
        _status.badgeMode    = false;
        _status.ssid.clear();
        _status.ipAddress.clear();
        _status.apSsid.clear();
        _status.apUrl.clear();
        _status.message = message.empty() ? "Connecting" : message;
    }

    void start_config_ap(const std::string& reason)
    {
        if (!ensure_initialized()) {
            return;
        }

        {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            if (_badge_active || !_wifi_enabled) {
                return;
            }
        }

        auto& wifi = WifiManager::GetInstance();
        wifi.StopStation();
        wifi.StartConfigAp();

        std::lock_guard<std::recursive_mutex> lock(_mutex);
        _station_deadline_ms = 0;
        _status.enabled      = true;
        _status.mode         = Hal::WifiMode::ConfigAp;
        _status.connected    = false;
        _status.configMode   = true;
        _status.badgeMode    = false;
        _status.ssid.clear();
        _status.ipAddress.clear();
        _status.apSsid  = wifi.GetApSsid();
        _status.apUrl   = wifi.GetApWebUrl();
        _status.message = reason.empty() ? "Setup AP" : reason;
    }

    void stop_wifi_owner()
    {
        if (!_initialized) {
            return;
        }

        auto& wifi = WifiManager::GetInstance();
        wifi.StopStation();
        wifi.StopConfigAp();
    }

    void set_off_status(const std::string& message)
    {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        _station_deadline_ms = 0;
        _status.enabled      = _wifi_enabled;
        _status.mode         = Hal::WifiMode::Off;
        _status.connected    = false;
        _status.configMode   = false;
        _status.badgeMode    = false;
        _status.ssid.clear();
        _status.ipAddress.clear();
        _status.apSsid.clear();
        _status.apUrl.clear();
        _status.message = message;
    }

    void handle_event(WifiEvent event, const std::string& data)
    {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        if (_badge_active) {
            return;
        }

        switch (event) {
        case WifiEvent::Scanning:
            if (_wifi_enabled && _status.mode == Hal::WifiMode::Connecting) {
                _status.message = "Scanning";
            }
            break;
        case WifiEvent::Connecting:
            if (_wifi_enabled) {
                _status.mode      = Hal::WifiMode::Connecting;
                _status.connected = false;
                _status.ssid      = data;
                _status.message   = "Connecting";
            }
            break;
        case WifiEvent::Connected:
            if (_wifi_enabled) {
                auto& wifi              = WifiManager::GetInstance();
                _station_deadline_ms    = 0;
                _status.mode            = Hal::WifiMode::Connected;
                _status.connected       = true;
                _status.configMode      = false;
                _status.ssid            = data.empty() ? wifi.GetSsid() : data;
                _status.ipAddress       = wifi.GetIpAddress();
                _status.message         = "Connected";
            }
            break;
        case WifiEvent::Disconnected:
            if (_wifi_enabled && _status.mode == Hal::WifiMode::Connected) {
                _status.mode      = Hal::WifiMode::Connecting;
                _status.connected = false;
                _status.message   = "Disconnected";
                _station_deadline_ms = millis() + _station_connect_timeout_ms;
            }
            break;
        case WifiEvent::ConfigModeEnter:
            if (_wifi_enabled) {
                auto& wifi        = WifiManager::GetInstance();
                _status.mode      = Hal::WifiMode::ConfigAp;
                _status.connected = false;
                _status.configMode = true;
                _status.apSsid    = wifi.GetApSsid();
                _status.apUrl     = wifi.GetApWebUrl();
                _status.message   = "Setup AP";
            }
            break;
        case WifiEvent::ConfigModeExit:
            _status.configMode = false;
            if (_wifi_enabled) {
                _restart_station_requested = true;
                _status.mode               = Hal::WifiMode::Connecting;
                _status.message            = "Connecting";
            }
            break;
        }
    }

    static NetworkController& controller()
    {
        static NetworkController instance;
        return instance;
    }

    std::recursive_mutex _mutex;
    bool _initialized                = false;
    bool _wifi_enabled               = false;
    bool _badge_active               = false;
    bool _restart_station_requested  = false;
    uint32_t _station_deadline_ms    = 0;
    Hal::WifiStatus _status;

    friend NetworkController& get_network_controller();
};

NetworkController& get_network_controller()
{
    return NetworkController::controller();
}

}  // namespace

void Hal::wifi_init()
{
    get_network_controller().init();
}

void Hal::wifi_tick()
{
    get_network_controller().tick();
}

void Hal::setWifiEnabled(bool enabled, bool saveToSettings)
{
    get_network_controller().set_enabled(enabled, saveToSettings);
}

bool Hal::isWifiEnabled(bool loadFromSettings)
{
    return get_network_controller().is_enabled(loadFromSettings);
}

Hal::WifiStatus Hal::getWifiStatus()
{
    return get_network_controller().status();
}

void Hal::wifi_enter_badge_ap()
{
    get_network_controller().enter_badge_ap();
}

void Hal::wifi_exit_badge_ap()
{
    get_network_controller().exit_badge_ap();
}
