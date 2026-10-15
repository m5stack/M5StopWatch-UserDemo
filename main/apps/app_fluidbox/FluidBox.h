// Keep the adapted FluidBox core close to its upstream source layout.
// clang-format off

#pragma once

#include <M5GFX.h>
#include <freertos/FreeRTOS.h>
#include <stddef.h>
#include <stdint.h>

#include "FluidBoxConfig.h"

class FluidBox {
 public:
  struct Stats {
    uint16_t pairCount = 0;
    bool pairOverflow = false;
    uint32_t droppedPairCount = 0;
    uint32_t pairOverflowEvents = 0;
    uint32_t nonFiniteResetCount = 0;
    float meanDensity = 0.0f;
    float restDensity = 0.0f;
    float meanSpeed = 0.0f;
    float maxSpeed = 0.0f;
    uint32_t stepMicros = 0;
  };

  struct RenderTiming {
    uint32_t projectionMicros = 0;
    uint32_t clearMicros = 0;
    uint32_t particleMicros = 0;
    bool snapshotAvailable = false;
  };

  bool begin(int16_t displayWidth, int16_t displayHeight);
  void reset();

  // accel is in g and gyro is in degrees/second, matching M5Unified.
  void setImuSample(float ax, float ay, float az,
                    float gx, float gy, float gz,
                    float sampleDtSeconds);
  bool applyTouchImpulse(float screenX, float screenY);
  void step(float realDtSeconds);

  // The caller owns startWrite()/endWrite() so one complete fluid frame is
  // committed to the M5GFX framebuffer in a single display transaction.
  RenderTiming render(LGFX_Device& display);

  const Stats& stats() const { return stats_; }
  bool imuActive() const { return imuActive_; }

 private:
  struct Vec3 {
    float x;
    float y;
    float z;
  };

  struct Particle {
    Vec3 position;
    Vec3 velocity;
    Vec3 previous;
    float density;
    float nearDensity;
    float speed;
  };

  struct NeighborPair {
    uint16_t a;
    uint16_t b;
  };

  struct ProjectedParticle {
    int16_t x;
    int16_t y;
    uint8_t radius;
    uint8_t depthLevel;
    uint16_t color;
    uint16_t highlight;
  };

  struct RenderParticle {
    Vec3 position;
    float speed;
  };

  enum class SnapshotState : uint8_t {
    kFree,
    kWriting,
    kPublished,
    kReading,
  };

  static constexpr int kGridCellCount =
      fluidbox_config::kGridX * fluidbox_config::kGridY * fluidbox_config::kGridZ;

  Particle particles_[fluidbox_config::kParticleCount]{};
  Vec3 viscosityDelta_[fluidbox_config::kParticleCount]{};
  float pressure_[fluidbox_config::kParticleCount]{};
  float nearPressure_[fluidbox_config::kParticleCount]{};

  int16_t cellHead_[kGridCellCount]{};
  int16_t cellNext_[fluidbox_config::kParticleCount]{};
  NeighborPair pairs_[fluidbox_config::kMaxNeighborPairs]{};
  size_t pairCount_ = 0;

  ProjectedParticle projected_[fluidbox_config::kParticleCount]{};
  uint16_t drawOrder_[fluidbox_config::kParticleCount]{};
  uint16_t colorLut_[fluidbox_config::kDepthLevels][fluidbox_config::kSpeedLevels]{};
  uint16_t highlightLut_[fluidbox_config::kDepthLevels][fluidbox_config::kSpeedLevels]{};

  // Physics publishes immutable snapshots; the render task owns one while it
  // draws. Three buffers let a newer simulation step replace a queued frame
  // without ever overwriting the frame currently read on the other core.
  static constexpr uint8_t kSnapshotCount = 3;
  RenderParticle renderSnapshots_[kSnapshotCount][fluidbox_config::kParticleCount]{};
  SnapshotState snapshotStates_[kSnapshotCount] = {
      SnapshotState::kFree, SnapshotState::kFree, SnapshotState::kFree};
  int8_t publishedSnapshot_ = -1;
  portMUX_TYPE snapshotMux_ = portMUX_INITIALIZER_UNLOCKED;

  int16_t width_ = 0;
  int16_t height_ = 0;
  float centerX_ = 0.0f;
  float centerY_ = 0.0f;
  float boxRadius_ = 0.0f;
  float restDensity_ = 0.0f;

  Vec3 gravity_ = {0.0f, fluidbox_config::kGravityAcceleration, 0.0f};
  Vec3 omega_ = {0.0f, 0.0f, 0.0f};
  Vec3 alpha_ = {0.0f, 0.0f, 0.0f};
  Vec3 lowPassAccel_ = {0.0f, 0.0f, 0.0f};
  Vec3 previousOmega_ = {0.0f, 0.0f, 0.0f};
  bool imuPrimed_ = false;
  bool imuActive_ = false;

  Stats stats_{};
  uint32_t pairOverflowEvents_ = 0;
  uint32_t nonFiniteResetCount_ = 0;
  uint32_t rngState_ = 0x2545F491u;

  float randomUnit();
  void calibrateRestDensity();
  void buildColorLuts();
  void rebuildGrid();
  void findPairsAndDensity(float dt);
  void relaxPositions(float dt);
  void resolveWalls();
  void integrateVelocities(float dt);
  bool stateIsFinite() const;
  int cellIndexFor(const Vec3& position) const;
  void publishRenderSnapshot();
  int acquireRenderSnapshot();
  void releaseRenderSnapshot(uint8_t snapshotIndex);
  void prepareProjection(const RenderParticle* particles);

  static uint16_t rgb565(int r, int g, int b);
};

// clang-format on
