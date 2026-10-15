/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <smooth_lvgl.hpp>
#include <cstdint>
#include <memory>
#include <vector>

namespace view {

/**
 * @brief
 *
 */
class WatchFaceBase {
public:
    virtual ~WatchFaceBase()                = default;
    virtual void onCreate(lv_obj_t* parent) = 0;
    virtual void onUpdate()                 = 0;
    virtual void onDestroy()                = 0;
};

/**
 * @brief
 *
 */
class WatchFaceClassic : public WatchFaceBase {
public:
    void onCreate(lv_obj_t* parent) override;
    void onUpdate() override;
    void onDestroy() override;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _click_mask;
    std::unique_ptr<uitk::lvgl_cpp::Container> _sub_panel;
    std::unique_ptr<uitk::lvgl_cpp::Label> _time_label;
    std::unique_ptr<uitk::lvgl_cpp::Label> _weekday_label;
    std::unique_ptr<uitk::lvgl_cpp::Container> _date_panel;
    std::unique_ptr<uitk::lvgl_cpp::Label> _date_label;
    std::unique_ptr<uitk::lvgl_cpp::Canvas> _ticks;
    std::unique_ptr<uitk::lvgl_cpp::Image> _hour_hand;
    std::unique_ptr<uitk::lvgl_cpp::Image> _minute_hand;
    std::unique_ptr<uitk::lvgl_cpp::Image> _second_hand;
    uint32_t _last_tick = 0;
    int _display_mode   = 0;

    void update();
    void cycle_display_mode();
    void apply_display_mode();
};

class WatchFaceBigNumber : public WatchFaceBase {
public:
    void onCreate(lv_obj_t* parent) override;
    void onUpdate() override;
    void onDestroy() override;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _click_mask;
    std::unique_ptr<uitk::lvgl_cpp::Image> _hour_tens;
    std::unique_ptr<uitk::lvgl_cpp::Image> _hour_ones;
    std::unique_ptr<uitk::lvgl_cpp::Image> _minute_tens;
    std::unique_ptr<uitk::lvgl_cpp::Image> _minute_ones;
    uint32_t _last_tick = 0;
    int _palette_index  = 0;

    void update();
    void cycle_palette();
    void apply_palette();
};

class WatchFaceNumberFlow : public WatchFaceBase {
public:
    void onCreate(lv_obj_t* parent) override;
    void onUpdate() override;
    void onDestroy() override;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _background_panel;
    std::unique_ptr<uitk::lvgl_cpp::Container> _click_mask;
    std::unique_ptr<uitk::lvgl_cpp::Container> _hour_panel;
    std::unique_ptr<uitk::lvgl_cpp::Container> _minute_panel;
    std::unique_ptr<uitk::lvgl_cpp::Container> _second_panel;
    std::unique_ptr<uitk::lvgl_cpp::NumberFlow> _hour_flow;
    std::unique_ptr<uitk::lvgl_cpp::NumberFlow> _minute_flow;
    std::unique_ptr<uitk::lvgl_cpp::NumberFlow> _second_flow;
    std::unique_ptr<uitk::lvgl_cpp::Label> _date_label;
    uint32_t _last_tick = 0;
    int _theme_index    = 0;

    void update();
    void cycle_theme();
    void apply_theme();
};

class WatchFaceSimple : public WatchFaceBase {
public:
    void onCreate(lv_obj_t* parent) override;
    void onUpdate() override;
    void onDestroy() override;

private:
    static void handleLongPressed(lv_event_t* e);

    std::unique_ptr<uitk::lvgl_cpp::Container> _background_panel;
    std::unique_ptr<uitk::lvgl_cpp::Container> _click_mask;
    std::unique_ptr<uitk::lvgl_cpp::Label> _time_label;
    std::unique_ptr<uitk::lvgl_cpp::Label> _date_label;
    std::unique_ptr<uitk::lvgl_cpp::Label> _tips_label;
    std::unique_ptr<uitk::lvgl_cpp::Container> _second_ball;
    uitk::AnimateValue _orbit_angle_anim;
    uint32_t _last_tick           = 0;
    uint32_t _tips_hide_tick      = 0;
    int _theme_index              = 0;
    bool _show_second_ball        = true;
    bool _suppress_next_click     = false;
    bool _show_tips               = true;
    bool _orbit_angle_initialized = false;
    float _orbit_angle_unwrapped  = 0.0f;

    void update();
    void apply_second_ball_state();
    void setOrbitAngle(float angle);
    void toggle_second_ball();
    void cycle_theme();
    void apply_theme();
};

class WatchFaceManager {
public:
    ~WatchFaceManager();

    void init();
    void update();
    void goNext();
    void goPrevious();

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _panel;
    std::vector<std::unique_ptr<WatchFaceBase>> _watch_faces;
    int _next_index                 = -1;
    int _current_index              = -1;
    bool _gesture_pressing          = false;
    lv_point_t _gesture_start_point = {0, 0};
    lv_point_t _gesture_last_point  = {0, 0};
    float _gesture_start_orientation_degrees = 0.0f;

    float _orientation_filtered_x = 0.0f;
    float _orientation_filtered_y = -1.0f;
    float _orientation_angle_degrees = 0.0f;
    uint32_t _orientation_last_tick = 0;
    bool _orientation_initialized = false;

    void update_orientation();
    void update_gesture();
};

}  // namespace view
