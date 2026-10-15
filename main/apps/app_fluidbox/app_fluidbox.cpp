/*
 * FluidBox integration for the M5Stack StopWatch demo.
 *
 * The simulation core is adapted from Flowduino/STOPWATCH-FluidBox.
 */
#include "app_fluidbox.h"
#include "FluidBoxConfig.h"

#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <esp_timer.h>
#include <new>

namespace cfg = fluidbox_config;

namespace {

constexpr int64_t periodMicros(uint16_t rate)
{
    return 1000000LL / rate;
}

}  // namespace

AppFluidBox::AppFluidBox()
{
    setAppInfo().name = "FluidBox";

    setAppInfo().icon = (void*)&fluidbox_icon;
}

void AppFluidBox::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppFluidBox::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _key_manager = std::make_unique<input::KeyManager>();

    // FluidBox draws directly with M5GFX. Stop the LVGL worker and take/release
    // its mutex once as a barrier so no in-flight LVGL flush can overlap our
    // direct framebuffer ownership.
    GetHAL().stopLvglUpdate();
    if (GetHAL().lvglLock()) {
        GetHAL().lvglUnlock();
    }

    auto& display = GetHAL().getDisplay();
    display.startWrite();
    display.fillScreen(TFT_BLACK);
    display.endWrite();

    // The solver owns more than 100 KB of fixed working arrays, so keep it
    // allocated only while this demo is actually open.
    _fluid.reset(new (std::nothrow) FluidBox());
    _fluid_ready = _fluid && _fluid->begin(display.width(), display.height());
    if (!_fluid) {
        mclog::tagError(getAppInfo().name, "failed to allocate FluidBox solver");
    }

    const int64_t now = esp_timer_get_time();
    _next_imu_micros = now;
    _next_simulation_micros = now;
    _next_render_micros = now;
    _last_imu_micros = 0;
    _touch_down = false;

    _render_stopped = xSemaphoreCreateBinary();
    _render_running.store(_fluid_ready);

    if (_fluid_ready && _render_stopped != nullptr) {
        const BaseType_t app_core = xPortGetCoreID();
        const BaseType_t render_core = app_core == 0 ? 1 : 0;
        const BaseType_t result = xTaskCreatePinnedToCore(
            renderTaskEntry,
            "fluid-render",
            cfg::kRenderTaskStackBytes,
            this,
            cfg::kRenderTaskPriority,
            &_render_task,
            render_core);
        _render_task_created = result == pdPASS;
    } else {
        _render_task_created = false;
    }

    mclog::tagInfo(getAppInfo().name, "fluid ready: {}, render task: {}",
                   _fluid_ready, _render_task_created);
}

void AppFluidBox::onRunning()
{
    GetHAL().updateButtonStates();
    const auto event = _key_manager ? _key_manager->update(false) : input::KeyEvent::None;

    if (event == input::KeyEvent::GoHome) {
        close();
        return;
    }

    // Match the standalone FluidBox recovery control: B resets the fluid.
    if (event == input::KeyEvent::GoNext && _fluid_ready) {
        _fluid->reset();
    }

    updateTouch();

    int64_t now = esp_timer_get_time();

    if (_fluid_ready && now >= _next_imu_micros) {
        _next_imu_micros = now + periodMicros(cfg::kImuHz);
        updateImu(now);
    }

    now = esp_timer_get_time();
    if (_fluid_ready && now >= _next_simulation_micros) {
        _next_simulation_micros = now + periodMicros(cfg::kSimulationHz);
        _fluid->step(1.0f / static_cast<float>(cfg::kSimulationHz));
    }

    // If task allocation failed, retain a usable single-task fallback.
    now = esp_timer_get_time();
    if (_fluid_ready && !_render_task_created && now >= _next_render_micros) {
        _next_render_micros = now + periodMicros(cfg::kRenderHz);
        renderFrame();
    }
}

void AppFluidBox::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    _render_running.store(false);

    if (_render_task_created && _render_stopped != nullptr) {
        xSemaphoreTake(_render_stopped, portMAX_DELAY);
    }

    _render_task = nullptr;
    _render_task_created = false;

    if (_render_stopped != nullptr) {
        vSemaphoreDelete(_render_stopped);
        _render_stopped = nullptr;
    }

    _fluid_ready = false;
    _touch_down = false;
    _fluid.reset();
    _key_manager.reset();

    auto& display = GetHAL().getDisplay();
    display.startWrite();
    display.fillScreen(TFT_BLACK);
    display.endWrite();

    // Hand display ownership back to LVGL. Mark the active screen dirty so the
    // launcher repaint is never dependent on stale framebuffer contents.
    if (GetHAL().lvglLock()) {
        lv_obj_invalidate(lv_screen_active());
        GetHAL().lvglUnlock();
    }
    GetHAL().startLvglUpdate();
}

void AppFluidBox::renderTaskEntry(void* arg)
{
    auto* self = static_cast<AppFluidBox*>(arg);
    self->renderLoop();

    if (self->_render_stopped != nullptr) {
        xSemaphoreGive(self->_render_stopped);
    }
    vTaskDelete(nullptr);
}

void AppFluidBox::renderLoop()
{
    const int64_t period = periodMicros(cfg::kRenderHz);
    int64_t nextFrameMicros = esp_timer_get_time();

    while (_render_running.load()) {
        const int64_t now = esp_timer_get_time();
        if (now < nextFrameMicros) {
            const int64_t remainingMicros = nextFrameMicros - now;
            const TickType_t remainingTicks =
                pdMS_TO_TICKS(static_cast<uint32_t>(remainingMicros / 1000));
            if (remainingTicks > 1) {
                vTaskDelay(remainingTicks - 1);
            } else {
                vTaskDelay(1);
            }
            continue;
        }

        // Drop missed frames instead of trying to catch up in a burst.
        nextFrameMicros = now + period;
        renderFrame();

        // Guarantee the core's idle task gets scheduling time.
        vTaskDelay(1);
    }
}

void AppFluidBox::renderFrame()
{
    auto& display = GetHAL().getDisplay();

    display.startWrite();
    if (_fluid_ready) {
        _fluid->render(display);
    } else {
        display.fillScreen(TFT_BLACK);
    }
    display.endWrite();
}

void AppFluidBox::updateImu(int64_t nowMicros)
{
    GetHAL().updateImuData();
    const auto& imu = GetHAL().getImuData();

    float dt = 1.0f / static_cast<float>(cfg::kImuHz);
    if (_last_imu_micros != 0 && nowMicros > _last_imu_micros) {
        dt = static_cast<float>(nowMicros - _last_imu_micros) * 1.0e-6f;
    }
    _last_imu_micros = nowMicros;

    // Hal::updateImuData() already exposes the BMI270 in the StopWatch demo's
    // display-oriented X/Y convention, so FluidBoxConfig disables its former
    // M5Unified-side X/Y swap.
    _fluid->setImuSample(
        imu.accelX,
        imu.accelY,
        imu.accelZ,
        imu.gyroX,
        imu.gyroY,
        imu.gyroZ,
        dt);
}

void AppFluidBox::updateTouch()
{
    const auto touch = GetHAL().getTouchPoint();
    const bool touch_down = touch.num > 0 && touch.x >= 0 && touch.y >= 0;

    if (touch_down && !_touch_down && _fluid_ready) {
        const bool displaced =
            _fluid->applyTouchImpulse(static_cast<float>(touch.x), static_cast<float>(touch.y));
        if (displaced && cfg::kTouchVibrationDurationMs > 0 &&
            cfg::kTouchVibrationStrength > 0) {
            GetHAL().vibrate(cfg::kTouchVibrationDurationMs, cfg::kTouchVibrationStrength);
        }
    }

    _touch_down = touch_down;
}
