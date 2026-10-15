/*
 * FluidBox integration for the M5Stack StopWatch demo.
 *
 * The simulation core is adapted from Flowduino/STOPWATCH-FluidBox.
 */
#pragma once

#include "FluidBox.h"
#include <apps/common/key_manager/key_manager.h>
#include <mooncake.h>
#include <atomic>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class AppFluidBox : public mooncake::AppAbility {
public:
    AppFluidBox();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::unique_ptr<input::KeyManager> _key_manager;
    std::unique_ptr<FluidBox> _fluid;
    bool _fluid_ready = false;

    std::atomic<bool> _render_running{false};
    bool _render_task_created = false;
    TaskHandle_t _render_task = nullptr;
    SemaphoreHandle_t _render_stopped = nullptr;

    int64_t _next_imu_micros = 0;
    int64_t _next_simulation_micros = 0;
    int64_t _next_render_micros = 0;
    int64_t _last_imu_micros = 0;
    bool _touch_down = false;

    static void renderTaskEntry(void* arg);
    void renderLoop();
    void renderFrame();
    void updateImu(int64_t nowMicros);
    void updateTouch();
};
