// Keep the adapted FluidBox core close to its upstream source layout.
// clang-format off

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fluidbox_config {

// Performance/quality. The solver uses fixed-size arrays, so changing this
// value changes both CPU cost and static RAM use without fragmenting the heap.
constexpr size_t kParticleCount = 450;
constexpr size_t kMaxNeighborPairs = kParticleCount * 32;
constexpr uint16_t kSimulationHz = 80;
// A measured frame costs about 32 ms, so 30 Hz is the highest safe initial
// target once rendering runs independently on the other core.
constexpr uint16_t kRenderHz = 25;
constexpr uint16_t kImuHz = 200;
constexpr uint8_t kRenderTaskPriority = 1;
constexpr uint32_t kRenderTaskStackBytes = 6144;

// 3-D neighbour grid. Each cell must remain at least kSmoothingRadius wide.
constexpr int kGridX = 10;
constexpr int kGridY = 10;
constexpr int kGridZ = 2;

// Active Water tuning group (future Oil/Jelly presets can replace this block).
// Keep these coupled values together and change one category per measurement.
constexpr float kRestSpacing = 27.0f;
constexpr float kSmoothingRadius = 45.0f;
constexpr float kTimeScale = 0.170f;
constexpr float kMaxScaledDt = 0.0022f;
constexpr float kPressure = 400000.0f;
constexpr float kNearPressure = 800000.0f;
constexpr float kMaxPairDisplacement = 3.5f;
constexpr float kViscositySigma = 38.0f;
constexpr float kViscosityBeta = 0.025f;

// The M5StopWatch is round, so the virtual enclosure is a shallow cylinder
// with rounded front/back joins instead of FluidBox's rectangular enclosure.
constexpr float kBoxRadiusRatio = 0.485f;
constexpr float kBoxDepth = 96.0f;
constexpr float kWallMargin = 5.0f;
constexpr float kFrontFillet = 8.0f;
constexpr float kBackFillet = 24.0f;
constexpr float kWallRestitution = 0.20f;
constexpr float kWallFriction = 0.965f;
constexpr float kWallJitter = 0.20f;

// Motion. Acceleration samples supplied by the StopWatch HAL are in g; gyro samples
// are degrees/second. Low-pass acceleration gives the gravity direction and
// the high-pass remainder supplies shake inertia.
constexpr float kGravityAcceleration = 140000.0f;
constexpr float kShakeAccelerationPerG = 90000.0f;
constexpr float kGravityLowPassHz = 1.2f;
constexpr float kRotationGain = 0.65f;

// Touch interaction. A tap applies a one-shot radial impulse to particles
// whose projected screen position falls within this radius.
constexpr float kTouchRadius = 142.0f;
constexpr float kTouchForce = 6000.0f;
constexpr uint16_t kTouchVibrationDurationMs = 45;
constexpr uint8_t kTouchVibrationStrength = 70;

// The official M5StopWatch demo swaps BMI270 X/Y when exposing display axes.
// These switches make real-device correction possible without solver edits.
// The shipping StopWatch HAL already swaps BMI270 X/Y into display axes.
constexpr bool kImuSwapXY = false;
constexpr bool kImuInvertX = false;
constexpr bool kImuInvertY = false;
constexpr bool kImuInvertZ = false;

// Projection and particle appearance.
constexpr float kProjectionFocal = 260.0f;
constexpr float kParticleRadius = 8.25f;
constexpr int kMaxDrawRadius = 11;
constexpr int kSpeedLevels = 64;
constexpr int kDepthLevels = 16;
constexpr float kSpeedColorMax = 5000.0f;
constexpr float kSpeedColorGamma = 0.55f;
constexpr float kFarDepthBrightness = 0.60f;
constexpr bool kHighlightsEnabled = true;
constexpr float kHighlightLift = 0.55f;

// Display. Rotation zero matches the M5StopWatch factory coordinate system.
constexpr uint8_t kDisplayRotation = 0;
constexpr uint8_t kDisplayBrightness = 230;
constexpr uint32_t kSerialStatsPeriodMs = 1000;

// Keep parameter edits inside the assumptions used by fixed-size storage,
// scheduler division, grid indexing, and LUT interpolation.
static_assert(kParticleCount > 0 && kParticleCount <= 32767,
              "particle count must fit the signed grid links");
static_assert(kMaxNeighborPairs <= 65535,
              "neighbor pair count must fit the public statistics type");
static_assert(kSimulationHz > 0 && kRenderHz > 0 && kImuHz > 0,
              "scheduler rates must be non-zero");
static_assert(kGridX > 0 && kGridY > 0 && kGridZ > 0,
              "grid dimensions must be non-zero");
static_assert(kSmoothingRadius > 0.0f && kRestSpacing > 0.0f,
              "solver radii must be positive");
static_assert(kTouchRadius > 0.0f && kTouchForce >= 0.0f,
              "touch interaction values must be non-negative and use a positive radius");
static_assert(kTouchVibrationStrength <= 100,
              "touch vibration strength must be between 0 and 100");
static_assert(kBoxDepth > 2.0f * kWallMargin,
              "virtual depth must leave usable interior space");
static_assert(kDepthLevels > 1 && kSpeedLevels > 1,
              "colour LUTs require at least two levels");
static_assert(kSpeedColorMax > 0.0f && kProjectionFocal > 0.0f,
              "projection and colour scales must be positive");

}  // namespace fluidbox_config

// clang-format on
