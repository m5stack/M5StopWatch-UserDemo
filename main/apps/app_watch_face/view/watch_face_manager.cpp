/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "watch_face.h"
#include <hal/hal.h>
#include <string_view>
#include <mooncake_log.h>
#include <cmath>

using namespace view;
using namespace uitk;
using namespace uitk::lvgl_cpp;

static const std::string_view _tag          = "WatchFaceManager";
static constexpr int _gesture_min_distance    = 60;
static constexpr int _panel_size              = 466;
static constexpr uint32_t _orientation_period_ms = 1;
static constexpr float _orientation_min_planar_g = 0.30f;
static constexpr float _orientation_filter_alpha = 0.50f;
static constexpr float _orientation_deadband_degrees = 0.25f;
static constexpr float _pi                    = 3.14159265358979323846f;
static constexpr float _radians_to_degrees    = 180.0f / _pi;
static constexpr float _degrees_to_radians    = _pi / 180.0f;

WatchFaceManager::~WatchFaceManager()
{
    int watch_face_count = static_cast<int>(_watch_faces.size());
    if (_current_index >= 0 && _current_index < watch_face_count) {
        // Clean up the active watch face before manager is destroyed.
        _watch_faces[_current_index]->onDestroy();
    }
}

void WatchFaceManager::init()
{
    mclog::tagInfo(_tag, "init");

    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setSize(_panel_size, _panel_size);
    _panel->setBgColor(lv_color_black());
    _panel->setPaddingAll(0);
    _panel->setBorderWidth(0);
    _panel->setRadius(0);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Rotate the entire watch-face tree around the physical centre of the
    // circular display. Individual watch faces therefore need no orientation
    // awareness of their own.
    lv_obj_set_style_transform_pivot_x(_panel->get(), _panel_size / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(_panel->get(), _panel_size / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(_panel->get(), 0, LV_PART_MAIN);

    _orientation_filtered_x = 0.0f;
    _orientation_filtered_y = -1.0f;
    _orientation_angle_degrees = 0.0f;
    _orientation_last_tick = 0;
    _orientation_initialized = false;

    // Register available watch faces here.
    _watch_faces.push_back(std::make_unique<WatchFaceClassic>());
    _watch_faces.push_back(std::make_unique<WatchFaceNumberFlow>());
    _watch_faces.push_back(std::make_unique<WatchFaceBigNumber>());
    _watch_faces.push_back(std::make_unique<WatchFaceSimple>());

    if (!_watch_faces.empty()) {
        // Show the first watch face on the next update tick.
        _next_index = 0;
    }
}

void WatchFaceManager::update()
{
    if (_watch_faces.empty()) {
        return;
    }

    update_orientation();
    update_gesture();

    if (_next_index >= 0 && _next_index != _current_index) {
        if (_current_index >= 0) {
            // Destroy the old watch face before switching.
            _watch_faces[_current_index]->onDestroy();
        }

        _current_index = _next_index;
        _next_index    = -1;

        // Create the new watch face after the index is updated.
        _watch_faces[_current_index]->onCreate(_panel->get());
    }

    if (_current_index >= 0) {
        // Update the active watch face every frame.
        _watch_faces[_current_index]->onUpdate();
    }
}

void WatchFaceManager::update_orientation()
{
    if (_panel == nullptr) {
        return;
    }

    const uint32_t now = GetHAL().millis();
    if (_orientation_last_tick != 0 &&
        now - _orientation_last_tick < _orientation_period_ms) {
        return;
    }
    _orientation_last_tick = now;

    GetHAL().updateImuData();
    const auto& imu = GetHAL().getImuData();

    float accel_x = imu.accelX;
    float accel_y = imu.accelY;
    const float planar_magnitude = std::sqrt(accel_x * accel_x + accel_y * accel_y);

    // When the display is close to face-up/face-down, gravity has too little
    // X/Y projection to define a trustworthy in-plane orientation. Keep the
    // last valid orientation instead of amplifying sensor noise.
    if (!std::isfinite(planar_magnitude) ||
        planar_magnitude < _orientation_min_planar_g) {
        return;
    }

    accel_x /= planar_magnitude;
    accel_y /= planar_magnitude;

    if (!_orientation_initialized) {
        _orientation_filtered_x = accel_x;
        _orientation_filtered_y = accel_y;
        _orientation_initialized = true;
    } else {
        _orientation_filtered_x +=
            (accel_x - _orientation_filtered_x) * _orientation_filter_alpha;
        _orientation_filtered_y +=
            (accel_y - _orientation_filtered_y) * _orientation_filter_alpha;
    }

    const float filtered_magnitude =
        std::sqrt(_orientation_filtered_x * _orientation_filtered_x +
                  _orientation_filtered_y * _orientation_filtered_y);
    if (filtered_magnitude < 1.0e-4f) {
        return;
    }

    const float desired_angle =
        std::atan2(_orientation_filtered_x, -_orientation_filtered_y) *
        _radians_to_degrees;

    float delta = desired_angle - _orientation_angle_degrees;
    while (delta > 180.0f) {
        delta -= 360.0f;
    }
    while (delta < -180.0f) {
        delta += 360.0f;
    }

    if (std::fabs(delta) < _orientation_deadband_degrees) {
        return;
    }

    _orientation_angle_degrees += delta;
    while (_orientation_angle_degrees > 180.0f) {
        _orientation_angle_degrees -= 360.0f;
    }
    while (_orientation_angle_degrees < -180.0f) {
        _orientation_angle_degrees += 360.0f;
    }

    lv_obj_set_style_transform_rotation(
        _panel->get(),
        static_cast<int32_t>(std::lround(_orientation_angle_degrees * 10.0f)),
        LV_PART_MAIN);
}

void WatchFaceManager::goNext()
{
    if (_watch_faces.empty()) {
        return;
    }

    int watch_face_count = static_cast<int>(_watch_faces.size());
    int base_index       = 0;

    if (_current_index >= 0) {
        base_index = _current_index;
    }

    _next_index = (base_index + 1) % watch_face_count;

    mclog::tagInfo(_tag, "go next watch face, next index: {}", _next_index);
}

void WatchFaceManager::goPrevious()
{
    if (_watch_faces.empty()) {
        return;
    }

    int watch_face_count = static_cast<int>(_watch_faces.size());
    int base_index       = 0;

    if (_current_index >= 0) {
        base_index = _current_index;
    }

    _next_index = base_index - 1;
    if (_next_index < 0) {
        _next_index = watch_face_count - 1;
    }

    mclog::tagInfo(_tag, "go previous watch face, next index: {}", _next_index);
}

void WatchFaceManager::update_gesture()
{
    lv_indev_t* indev = GetHAL().lvTouchpad;
    if (indev == nullptr) {
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    bool is_pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    if (is_pressed) {
        if (!_gesture_pressing) {
            // Start tracking from the first pressed point.
            _gesture_pressing                  = true;
            _gesture_start_point               = point;
            _gesture_start_orientation_degrees = _orientation_angle_degrees;
        }

        _gesture_last_point = point;
        return;
    }

    if (!_gesture_pressing) {
        return;
    }

    _gesture_pressing = false;

    const float raw_delta_x =
        static_cast<float>(_gesture_last_point.x - _gesture_start_point.x);
    const float raw_delta_y =
        static_cast<float>(_gesture_last_point.y - _gesture_start_point.y);

    // Convert the physical-screen gesture back into the rotated watch-face
    // coordinate system so "horizontal" continues to mean horizontal to the
    // user regardless of device orientation.
    const float radians = _gesture_start_orientation_degrees * _degrees_to_radians;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const int delta_x = static_cast<int>(std::lround(
        cosine * raw_delta_x + sine * raw_delta_y));
    const int delta_y = static_cast<int>(std::lround(
        -sine * raw_delta_x + cosine * raw_delta_y));
    const int abs_x = delta_x >= 0 ? delta_x : -delta_x;
    const int abs_y = delta_y >= 0 ? delta_y : -delta_y;

    // Only handle clear horizontal swipes.
    if (abs_x < _gesture_min_distance || abs_x <= abs_y) {
        return;
    }

    if (delta_x < 0) {
        goNext();
    } else {
        goPrevious();
    }
}
