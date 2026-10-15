/*
  M5StopWatch FluidBox simulation.

  The solver, motion decomposition, perspective cues, and LUT-based colour
  strategy are an Arduino/M5GFX adaptation inspired by V4C38/esp32-fluidbox
  (MIT License). Waveshare display, GPIO, IMU, power, DMA, and ESP-IDF startup
  code are intentionally not used. See THIRD_PARTY_NOTICES.md.
*/

// Keep the adapted FluidBox core close to its upstream source layout.
// clang-format off

#include "FluidBox.h"

#include <esp_timer.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

namespace cfg = fluidbox_config;

namespace {

uint32_t fluidboxMicros()
{
  return static_cast<uint32_t>(esp_timer_get_time());
}

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

float clampFloat(float value, float low, float high) {
  return value < low ? low : (value > high ? high : value);
}

int clampInt(int value, int low, int high) {
  return value < low ? low : (value > high ? high : value);
}

}  // namespace

bool FluidBox::begin(int16_t displayWidth, int16_t displayHeight) {
  if (displayWidth <= 0 || displayHeight <= 0) {
    return false;
  }

  width_ = displayWidth;
  height_ = displayHeight;
  centerX_ = 0.5f * static_cast<float>(width_ - 1);
  centerY_ = 0.5f * static_cast<float>(height_ - 1);
  const int16_t shorterSide = width_ < height_ ? width_ : height_;
  boxRadius_ = static_cast<float>(shorterSide) * cfg::kBoxRadiusRatio;

  const float cellW = static_cast<float>(width_) / cfg::kGridX;
  const float cellH = static_cast<float>(height_) / cfg::kGridY;
  const float cellD = cfg::kBoxDepth / cfg::kGridZ;
  if (cellW + 0.01f < cfg::kSmoothingRadius ||
      cellH + 0.01f < cfg::kSmoothingRadius ||
      cellD + 0.01f < cfg::kSmoothingRadius) {
    printf("[FluidBox] invalid grid %.1fx%.1fx%.1f for h=%.1f\n",
                  cellW, cellH, cellD, cfg::kSmoothingRadius);
    return false;
  }

  calibrateRestDensity();
  buildColorLuts();
  reset();

  printf("[FluidBox] %u particles, box radius %.1f depth %.1f, rest rho %.3f\n",
                static_cast<unsigned>(cfg::kParticleCount), boxRadius_,
                cfg::kBoxDepth, restDensity_);
  return true;
}

float FluidBox::randomUnit() {
  rngState_ ^= rngState_ << 13;
  rngState_ ^= rngState_ >> 17;
  rngState_ ^= rngState_ << 5;
  return static_cast<float>(rngState_ & 0x00FFFFFFu) * (1.0f / 16777215.0f);
}

void FluidBox::calibrateRestDensity() {
  const int reach = static_cast<int>(ceilf(cfg::kSmoothingRadius / cfg::kRestSpacing));
  restDensity_ = 0.0f;

  for (int x = -reach; x <= reach; ++x) {
    for (int y = -reach; y <= reach; ++y) {
      for (int z = -reach; z <= reach; ++z) {
        if (x == 0 && y == 0 && z == 0) {
          continue;
        }
        const float latticeDistance = cfg::kRestSpacing *
            sqrtf(static_cast<float>(x * x + y * y + z * z));
        if (latticeDistance >= cfg::kSmoothingRadius) {
          continue;
        }
        const float q = 1.0f - latticeDistance / cfg::kSmoothingRadius;
        restDensity_ += q * q;
      }
    }
  }
}

void FluidBox::reset() {
  const float sideRadius = boxRadius_ - cfg::kWallMargin;
  const float spacing = cfg::kRestSpacing;
  const float zFirst = cfg::kWallMargin + 0.5f * spacing;
  const float zLast = cfg::kBoxDepth - cfg::kWallMargin - 0.5f * spacing;
  size_t seeded = 0;

  // Fill from the physical bottom upward. Starting on a relaxed lattice avoids
  // spending the first seconds resolving a deliberately overpacked blob.
  for (float y = centerY_ + sideRadius - 0.5f * spacing;
       y >= centerY_ - sideRadius && seeded < cfg::kParticleCount;
       y -= spacing) {
    for (float x = centerX_ - sideRadius + 0.5f * spacing;
         x <= centerX_ + sideRadius && seeded < cfg::kParticleCount;
         x += spacing) {
      const float dx = x - centerX_;
      const float dy = y - centerY_;
      if (dx * dx + dy * dy >
          (sideRadius - 0.45f * spacing) * (sideRadius - 0.45f * spacing)) {
        continue;
      }

      for (float z = zFirst; z <= zLast + 0.01f && seeded < cfg::kParticleCount;
           z += spacing) {
        Particle& p = particles_[seeded++];
        const float jitter = spacing * 0.10f;
        p.position = {
            x + (randomUnit() - 0.5f) * jitter,
            y + (randomUnit() - 0.5f) * jitter,
            z + (randomUnit() - 0.5f) * jitter,
        };
        p.velocity = {0.0f, 0.0f, 0.0f};
        p.previous = p.position;
        p.density = restDensity_;
        p.nearDensity = 0.0f;
        p.speed = 0.0f;
      }
    }
  }

  // This is only a guard for unusually small displays or aggressive tuning.
  // It keeps every fixed-array entry valid without allocating fallback storage.
  while (seeded < cfg::kParticleCount) {
    const float angle = randomUnit() * 2.0f * kPi;
    const float radius = sqrtf(randomUnit()) * (sideRadius - spacing);
    Particle& p = particles_[seeded++];
    p.position = {
        centerX_ + cosf(angle) * radius,
        centerY_ + sinf(angle) * radius,
        cfg::kWallMargin + randomUnit() * (cfg::kBoxDepth - 2.0f * cfg::kWallMargin),
    };
    p.velocity = {0.0f, 0.0f, 0.0f};
    p.previous = p.position;
    p.density = restDensity_;
    p.nearDensity = 0.0f;
    p.speed = 0.0f;
  }

  pairCount_ = 0;
  stats_ = {};
  stats_.restDensity = restDensity_;
  stats_.pairOverflowEvents = pairOverflowEvents_;
  stats_.nonFiniteResetCount = nonFiniteResetCount_;
  publishRenderSnapshot();
}

void FluidBox::setImuSample(float ax, float ay, float az,
                            float gx, float gy, float gz,
                            float sampleDtSeconds) {
  if (!isfinite(ax) || !isfinite(ay) || !isfinite(az) ||
      !isfinite(gx) || !isfinite(gy) || !isfinite(gz)) {
    return;
  }

  float mappedAx = cfg::kImuSwapXY ? ay : ax;
  float mappedAy = cfg::kImuSwapXY ? ax : ay;
  float mappedAz = az;
  float mappedGx = cfg::kImuSwapXY ? gy : gx;
  float mappedGy = cfg::kImuSwapXY ? gx : gy;
  float mappedGz = gz;

  if (cfg::kImuInvertX) {
    mappedAx = -mappedAx;
    mappedGx = -mappedGx;
  }
  if (cfg::kImuInvertY) {
    mappedAy = -mappedAy;
    mappedGy = -mappedGy;
  }
  if (cfg::kImuInvertZ) {
    mappedAz = -mappedAz;
    mappedGz = -mappedGz;
  }

  const float accelMagnitude =
      sqrtf(mappedAx * mappedAx + mappedAy * mappedAy + mappedAz * mappedAz);
  if (accelMagnitude < 0.02f || accelMagnitude > 16.0f) {
    return;
  }

  const float dt = clampFloat(sampleDtSeconds, 0.001f, 0.20f);
  if (!imuPrimed_) {
    lowPassAccel_ = {mappedAx, mappedAy, mappedAz};
    imuPrimed_ = true;
  } else {
    const float coefficient = 1.0f - expf(-2.0f * kPi * cfg::kGravityLowPassHz * dt);
    lowPassAccel_.x += coefficient * (mappedAx - lowPassAccel_.x);
    lowPassAccel_.y += coefficient * (mappedAy - lowPassAccel_.y);
    lowPassAccel_.z += coefficient * (mappedAz - lowPassAccel_.z);
  }

  // At rest an accelerometer measures support opposite to gravity, hence the
  // minus sign. The high-pass remainder is enclosure acceleration; fluid in
  // the enclosure frame feels it in the opposite direction as well.
  Vec3 down = {-lowPassAccel_.x, -lowPassAccel_.y, -lowPassAccel_.z};
  const float downMagnitude = sqrtf(down.x * down.x + down.y * down.y + down.z * down.z);
  if (downMagnitude > 0.05f) {
    const float inverse = 1.0f / downMagnitude;
    down.x *= inverse;
    down.y *= inverse;
    down.z *= inverse;
  } else {
    down = {0.0f, 1.0f, 0.0f};
  }

  gravity_.x = down.x * cfg::kGravityAcceleration -
               (mappedAx - lowPassAccel_.x) * cfg::kShakeAccelerationPerG;
  gravity_.y = down.y * cfg::kGravityAcceleration -
               (mappedAy - lowPassAccel_.y) * cfg::kShakeAccelerationPerG;
  gravity_.z = down.z * cfg::kGravityAcceleration -
               (mappedAz - lowPassAccel_.z) * cfg::kShakeAccelerationPerG;

  omega_ = {mappedGx * kDegToRad, mappedGy * kDegToRad, mappedGz * kDegToRad};
  const float inverseDt = 1.0f / dt;
  alpha_.x = clampFloat((omega_.x - previousOmega_.x) * inverseDt, -200.0f, 200.0f);
  alpha_.y = clampFloat((omega_.y - previousOmega_.y) * inverseDt, -200.0f, 200.0f);
  alpha_.z = clampFloat((omega_.z - previousOmega_.z) * inverseDt, -200.0f, 200.0f);
  previousOmega_ = omega_;
  imuActive_ = true;
}

bool FluidBox::applyTouchImpulse(float screenX, float screenY) {
  if (!isfinite(screenX) || !isfinite(screenY) ||
      cfg::kTouchForce <= 0.0f || cfg::kTouchRadius <= 0.0f) {
    return false;
  }

  const float radiusSquared = cfg::kTouchRadius * cfg::kTouchRadius;
  bool affected = false;

  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    Particle& p = particles_[i];
    const float scale = cfg::kProjectionFocal /
                        (cfg::kProjectionFocal + p.position.z);
    if (scale <= 1.0e-4f) {
      continue;
    }

    const float projectedX = centerX_ + (p.position.x - centerX_) * scale;
    const float projectedY = centerY_ + (p.position.y - centerY_) * scale;
    const float dx = projectedX - screenX;
    const float dy = projectedY - screenY;
    const float distanceSquared = dx * dx + dy * dy;

    if (distanceSquared >= radiusSquared) {
      continue;
    }

    const float distance = sqrtf(distanceSquared);
    float directionX = 0.0f;
    float directionY = 0.0f;

    if (distance > 1.0e-3f) {
      const float inverseDistance = 1.0f / distance;
      directionX = dx * inverseDistance;
      directionY = dy * inverseDistance;
    } else {
      // Exact overlap is rare, but give coincident particles deterministic
      // radial directions instead of leaving them stationary.
      const float angle = static_cast<float>(i) * 2.39996323f;
      directionX = cosf(angle);
      directionY = sinf(angle);
    }

    const float normalizedDistance = distance / cfg::kTouchRadius;
    const float falloff = 1.0f - normalizedDistance;
    const float impulse = cfg::kTouchForce * falloff * falloff;

    // Compensate for perspective so the visible screen-space response is
    // reasonably consistent for particles at different depths.
    const float worldImpulse = impulse / scale;
    p.velocity.x += directionX * worldImpulse;
    p.velocity.y += directionY * worldImpulse;
    affected = true;
  }

  return affected;
}

int FluidBox::cellIndexFor(const Vec3& position) const {
  int cx = static_cast<int>(position.x * cfg::kGridX / static_cast<float>(width_));
  int cy = static_cast<int>(position.y * cfg::kGridY / static_cast<float>(height_));
  int cz = static_cast<int>(position.z * cfg::kGridZ / cfg::kBoxDepth);
  cx = clampInt(cx, 0, cfg::kGridX - 1);
  cy = clampInt(cy, 0, cfg::kGridY - 1);
  cz = clampInt(cz, 0, cfg::kGridZ - 1);
  return (cz * cfg::kGridY + cy) * cfg::kGridX + cx;
}

void FluidBox::rebuildGrid() {
  for (int cell = 0; cell < kGridCellCount; ++cell) {
    cellHead_[cell] = -1;
  }

  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    const int cell = cellIndexFor(particles_[i].position);
    cellNext_[i] = cellHead_[cell];
    cellHead_[cell] = static_cast<int16_t>(i);
  }
}

void FluidBox::findPairsAndDensity(float dt) {
  pairCount_ = 0;
  stats_.pairOverflow = false;
  stats_.droppedPairCount = 0;
  const float hSquared = cfg::kSmoothingRadius * cfg::kSmoothingRadius;
  const float inverseH = 1.0f / cfg::kSmoothingRadius;

  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    particles_[i].density = 0.0f;
    particles_[i].nearDensity = 0.0f;
    viscosityDelta_[i] = {0.0f, 0.0f, 0.0f};
  }

  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    const Particle& a = particles_[i];
    int cx = static_cast<int>(a.position.x * cfg::kGridX / static_cast<float>(width_));
    int cy = static_cast<int>(a.position.y * cfg::kGridY / static_cast<float>(height_));
    int cz = static_cast<int>(a.position.z * cfg::kGridZ / cfg::kBoxDepth);
    cx = clampInt(cx, 0, cfg::kGridX - 1);
    cy = clampInt(cy, 0, cfg::kGridY - 1);
    cz = clampInt(cz, 0, cfg::kGridZ - 1);

    for (int dzCell = -1; dzCell <= 1; ++dzCell) {
      const int nz = cz + dzCell;
      if (nz < 0 || nz >= cfg::kGridZ) {
        continue;
      }
      for (int dyCell = -1; dyCell <= 1; ++dyCell) {
        const int ny = cy + dyCell;
        if (ny < 0 || ny >= cfg::kGridY) {
          continue;
        }
        for (int dxCell = -1; dxCell <= 1; ++dxCell) {
          const int nx = cx + dxCell;
          if (nx < 0 || nx >= cfg::kGridX) {
            continue;
          }

          const int cell = (nz * cfg::kGridY + ny) * cfg::kGridX + nx;
          for (int j = cellHead_[cell]; j >= 0; j = cellNext_[j]) {
            if (j <= static_cast<int>(i)) {
              continue;
            }

            Particle& b = particles_[j];
            const float dx = b.position.x - a.position.x;
            const float dy = b.position.y - a.position.y;
            const float dz = b.position.z - a.position.z;
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            if (distanceSquared >= hSquared || distanceSquared < 1.0e-6f) {
              continue;
            }

            if (pairCount_ < cfg::kMaxNeighborPairs) {
              pairs_[pairCount_++] = {
                  static_cast<uint16_t>(i), static_cast<uint16_t>(j)};
            } else {
              stats_.pairOverflow = true;
              ++stats_.droppedPairCount;
            }

            const float distance = sqrtf(distanceSquared);
            const float inverseDistance = 1.0f / distance;
            const float q = 1.0f - distance * inverseH;
            const float qSquared = q * q;
            const float nearContribution = qSquared * q;
            particles_[i].density += qSquared;
            particles_[i].nearDensity += nearContribution;
            b.density += qSquared;
            b.nearDensity += nearContribution;

            const float ux = dx * inverseDistance;
            const float uy = dy * inverseDistance;
            const float uz = dz * inverseDistance;
            const float closingSpeed =
                (a.velocity.x - b.velocity.x) * ux +
                (a.velocity.y - b.velocity.y) * uy +
                (a.velocity.z - b.velocity.z) * uz;
            if (closingSpeed <= 0.0f) {
              continue;
            }

            float velocityChange = 0.5f * dt * q *
                (cfg::kViscositySigma * closingSpeed +
                 cfg::kViscosityBeta * closingSpeed * closingSpeed);
            if (velocityChange > 0.5f * closingSpeed) {
              velocityChange = 0.5f * closingSpeed;
            }
            const float positionImpulse = velocityChange * dt;
            viscosityDelta_[i].x -= positionImpulse * ux;
            viscosityDelta_[i].y -= positionImpulse * uy;
            viscosityDelta_[i].z -= positionImpulse * uz;
            viscosityDelta_[j].x += positionImpulse * ux;
            viscosityDelta_[j].y += positionImpulse * uy;
            viscosityDelta_[j].z += positionImpulse * uz;
          }
        }
      }
    }
  }

  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    particles_[i].position.x += viscosityDelta_[i].x;
    particles_[i].position.y += viscosityDelta_[i].y;
    particles_[i].position.z += viscosityDelta_[i].z;
  }

  if (stats_.pairOverflow) {
    ++pairOverflowEvents_;
  }
  stats_.pairOverflowEvents = pairOverflowEvents_;
}

void FluidBox::relaxPositions(float dt) {
  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    pressure_[i] = cfg::kPressure * (particles_[i].density - restDensity_);
    nearPressure_[i] = cfg::kNearPressure * particles_[i].nearDensity;
  }

  const float hSquared = cfg::kSmoothingRadius * cfg::kSmoothingRadius;
  const float inverseH = 1.0f / cfg::kSmoothingRadius;
  const float dtSquared = dt * dt;

  for (size_t pairIndex = 0; pairIndex < pairCount_; ++pairIndex) {
    const size_t i = pairs_[pairIndex].a;
    const size_t j = pairs_[pairIndex].b;
    const float dx = particles_[j].position.x - particles_[i].position.x;
    const float dy = particles_[j].position.y - particles_[i].position.y;
    const float dz = particles_[j].position.z - particles_[i].position.z;
    const float distanceSquared = dx * dx + dy * dy + dz * dz;
    if (distanceSquared >= hSquared || distanceSquared < 1.0e-6f) {
      continue;
    }

    const float distance = sqrtf(distanceSquared);
    const float inverseDistance = 1.0f / distance;
    const float q = 1.0f - distance * inverseH;
    float displacement = 0.5f * dtSquared *
        ((pressure_[i] + pressure_[j]) * q +
         (nearPressure_[i] + nearPressure_[j]) * q * q);
    displacement = clampFloat(displacement,
                              -cfg::kMaxPairDisplacement,
                              cfg::kMaxPairDisplacement);

    const float moveX = dx * inverseDistance * displacement;
    const float moveY = dy * inverseDistance * displacement;
    const float moveZ = dz * inverseDistance * displacement;
    particles_[i].position.x -= moveX;
    particles_[i].position.y -= moveY;
    particles_[i].position.z -= moveZ;
    particles_[j].position.x += moveX;
    particles_[j].position.y += moveY;
    particles_[j].position.z += moveZ;
  }
}

void FluidBox::integrateVelocities(float dt) {
  const Vec3 scaledOmega = {
      omega_.x * cfg::kTimeScale,
      omega_.y * cfg::kTimeScale,
      omega_.z * cfg::kTimeScale,
  };
  const Vec3 scaledAlpha = {
      alpha_.x * cfg::kTimeScale * cfg::kTimeScale,
      alpha_.y * cfg::kTimeScale * cfg::kTimeScale,
      alpha_.z * cfg::kTimeScale * cfg::kTimeScale,
  };
  const float omegaSquared = scaledOmega.x * scaledOmega.x +
                             scaledOmega.y * scaledOmega.y +
                             scaledOmega.z * scaledOmega.z;
  const bool rotating = cfg::kRotationGain != 0.0f &&
      (omegaSquared > 1.0e-8f ||
       scaledAlpha.x * scaledAlpha.x + scaledAlpha.y * scaledAlpha.y +
       scaledAlpha.z * scaledAlpha.z > 1.0e-8f);

  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    Particle& p = particles_[i];
    Vec3 acceleration = gravity_;

    if (rotating) {
      const Vec3 radius = {
          p.position.x - centerX_,
          p.position.y - centerY_,
          p.position.z - 0.5f * cfg::kBoxDepth,
      };
      const float omegaDotRadius = scaledOmega.x * radius.x +
                                   scaledOmega.y * radius.y +
                                   scaledOmega.z * radius.z;

      acceleration.x += cfg::kRotationGain *
          (radius.x * omegaSquared - scaledOmega.x * omegaDotRadius);
      acceleration.y += cfg::kRotationGain *
          (radius.y * omegaSquared - scaledOmega.y * omegaDotRadius);
      acceleration.z += cfg::kRotationGain *
          (radius.z * omegaSquared - scaledOmega.z * omegaDotRadius);

      acceleration.x -= cfg::kRotationGain *
          (scaledAlpha.y * radius.z - scaledAlpha.z * radius.y);
      acceleration.y -= cfg::kRotationGain *
          (scaledAlpha.z * radius.x - scaledAlpha.x * radius.z);
      acceleration.z -= cfg::kRotationGain *
          (scaledAlpha.x * radius.y - scaledAlpha.y * radius.x);

      acceleration.x -= cfg::kRotationGain * 2.0f *
          (scaledOmega.y * p.velocity.z - scaledOmega.z * p.velocity.y);
      acceleration.y -= cfg::kRotationGain * 2.0f *
          (scaledOmega.z * p.velocity.x - scaledOmega.x * p.velocity.z);
      acceleration.z -= cfg::kRotationGain * 2.0f *
          (scaledOmega.x * p.velocity.y - scaledOmega.y * p.velocity.x);
    }

    p.velocity.x += acceleration.x * dt;
    p.velocity.y += acceleration.y * dt;
    p.velocity.z += acceleration.z * dt;
  }
}

void FluidBox::resolveWalls() {
  const float sideRadius = boxRadius_ - cfg::kWallMargin;
  const float lowZ = cfg::kWallMargin;
  const float highZ = cfg::kBoxDepth - cfg::kWallMargin;

  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    Particle& p = particles_[i];
    const float radialX = p.position.x - centerX_;
    const float radialY = p.position.y - centerY_;
    const float radialDistance = sqrtf(radialX * radialX + radialY * radialY);
    float unitX = 0.0f;
    float unitY = 0.0f;
    if (radialDistance > 1.0e-6f) {
      unitX = radialX / radialDistance;
      unitY = radialY / radialDistance;
    }

    float fillet = 0.0f;
    float centerZ = p.position.z;
    if (p.position.z < lowZ + cfg::kFrontFillet) {
      fillet = cfg::kFrontFillet;
      centerZ = lowZ + fillet;
    } else if (p.position.z > highZ - cfg::kBackFillet) {
      fillet = cfg::kBackFillet;
      centerZ = highZ - fillet;
    }

    const float radialLimit = sideRadius - fillet;
    const float clampedRadius = radialDistance < radialLimit
                                    ? radialDistance
                                    : radialLimit;
    const float dr = radialDistance - clampedRadius;
    const float dz = p.position.z - centerZ;
    const float outsideSquared = dr * dr + dz * dz;
    if (outsideSquared <= fillet * fillet) {
      continue;
    }

    const float outside = sqrtf(outsideSquared);
    if (outside < 1.0e-6f) {
      continue;
    }
    const float normalRadius = dr / outside;
    const float normalZ = dz / outside;
    const float inset = fillet - cfg::kWallJitter * randomUnit();
    const float newRadius = clampedRadius + normalRadius * inset;

    p.position.x = centerX_ + unitX * newRadius;
    p.position.y = centerY_ + unitY * newRadius;
    p.position.z = centerZ + normalZ * inset;

    const float normalX = unitX * normalRadius;
    const float normalY = unitY * normalRadius;
    const float outwardSpeed = p.velocity.x * normalX +
                               p.velocity.y * normalY +
                               p.velocity.z * normalZ;
    if (outwardSpeed > 0.0f) {
      const float bounce = (1.0f + cfg::kWallRestitution) * outwardSpeed;
      p.velocity.x -= bounce * normalX;
      p.velocity.y -= bounce * normalY;
      p.velocity.z -= bounce * normalZ;
    }

    const float keepNormal = p.velocity.x * normalX +
                             p.velocity.y * normalY +
                             p.velocity.z * normalZ;
    const float restore = keepNormal * (1.0f - cfg::kWallFriction);
    p.velocity.x = p.velocity.x * cfg::kWallFriction + restore * normalX;
    p.velocity.y = p.velocity.y * cfg::kWallFriction + restore * normalY;
    p.velocity.z = p.velocity.z * cfg::kWallFriction + restore * normalZ;
  }
}

bool FluidBox::stateIsFinite() const {
  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    const Particle& p = particles_[i];
    if (!isfinite(p.position.x) || !isfinite(p.position.y) || !isfinite(p.position.z) ||
        !isfinite(p.velocity.x) || !isfinite(p.velocity.y) || !isfinite(p.velocity.z)) {
      return false;
    }
  }
  return true;
}

void FluidBox::step(float realDtSeconds) {
  const uint32_t startedAt = fluidboxMicros();
  if (!isfinite(realDtSeconds) || realDtSeconds <= 0.0f) {
    return;
  }

  float dt = realDtSeconds * cfg::kTimeScale;
  if (dt > cfg::kMaxScaledDt) {
    dt = cfg::kMaxScaledDt;
  }
  if (dt < 1.0e-5f) {
    return;
  }

  integrateVelocities(dt);
  const float inverseDt = 1.0f / dt;
  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    Particle& p = particles_[i];
    p.previous = p.position;
    p.position.x += p.velocity.x * dt;
    p.position.y += p.velocity.y * dt;
    p.position.z += p.velocity.z * dt;
  }

  rebuildGrid();
  findPairsAndDensity(dt);
  relaxPositions(dt);

  float densitySum = 0.0f;
  float speedSum = 0.0f;
  float maximumSpeed = 0.0f;
  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    Particle& p = particles_[i];
    p.velocity.x = (p.position.x - p.previous.x) * inverseDt;
    p.velocity.y = (p.position.y - p.previous.y) * inverseDt;
    p.velocity.z = (p.position.z - p.previous.z) * inverseDt;
  }

  resolveWalls();
  if (!stateIsFinite()) {
    printf("[FluidBox] non-finite solver state; reseeding\n");
    ++nonFiniteResetCount_;
    reset();
    return;
  }

  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    Particle& p = particles_[i];
    const float speed = sqrtf(p.velocity.x * p.velocity.x +
                              p.velocity.y * p.velocity.y +
                              p.velocity.z * p.velocity.z);
    p.speed = speed;
    densitySum += p.density;
    speedSum += speed;
    if (speed > maximumSpeed) {
      maximumSpeed = speed;
    }
  }

  const float inverseCount = 1.0f / static_cast<float>(cfg::kParticleCount);
  stats_.pairCount = static_cast<uint16_t>(pairCount_);
  stats_.meanDensity = densitySum * inverseCount;
  stats_.restDensity = restDensity_;
  stats_.meanSpeed = speedSum * inverseCount;
  stats_.maxSpeed = maximumSpeed;
  publishRenderSnapshot();
  stats_.stepMicros = fluidboxMicros() - startedAt;
}

uint16_t FluidBox::rgb565(int r, int g, int b) {
  r = clampInt(r, 0, 255);
  g = clampInt(g, 0, 255);
  b = clampInt(b, 0, 255);
  return static_cast<uint16_t>(((r & 0xF8) << 8) |
                               ((g & 0xFC) << 3) |
                               (b >> 3));
}

void FluidBox::buildColorLuts() {
  constexpr int kStops = 4;
  constexpr float stopPosition[kStops] = {0.00f, 0.45f, 0.78f, 1.00f};
  constexpr int stopColor[kStops][3] = {
      {10, 45, 165},
      {40, 125, 235},
      {150, 205, 250},
      {255, 255, 255},
  };

  for (int depth = 0; depth < cfg::kDepthLevels; ++depth) {
    const float depthPosition = static_cast<float>(depth) /
                                static_cast<float>(cfg::kDepthLevels - 1);
    const float depthBrightness = cfg::kFarDepthBrightness +
        (1.0f - cfg::kFarDepthBrightness) * (1.0f - depthPosition);

    for (int speed = 0; speed < cfg::kSpeedLevels; ++speed) {
      const float linearSpeed = static_cast<float>(speed) /
                                static_cast<float>(cfg::kSpeedLevels - 1);
      const float speedPosition = powf(linearSpeed, cfg::kSpeedColorGamma);
      int red = stopColor[kStops - 1][0];
      int green = stopColor[kStops - 1][1];
      int blue = stopColor[kStops - 1][2];

      for (int stop = 0; stop < kStops - 1; ++stop) {
        if (speedPosition <= stopPosition[stop + 1]) {
          const float amount = (speedPosition - stopPosition[stop]) /
              (stopPosition[stop + 1] - stopPosition[stop]);
          red = static_cast<int>(stopColor[stop][0] +
              amount * (stopColor[stop + 1][0] - stopColor[stop][0]));
          green = static_cast<int>(stopColor[stop][1] +
              amount * (stopColor[stop + 1][1] - stopColor[stop][1]));
          blue = static_cast<int>(stopColor[stop][2] +
              amount * (stopColor[stop + 1][2] - stopColor[stop][2]));
          break;
        }
      }

      colorLut_[depth][speed] = rgb565(
          static_cast<int>(red * depthBrightness),
          static_cast<int>(green * depthBrightness),
          static_cast<int>(blue * depthBrightness));

      const float lift = cfg::kHighlightLift;
      highlightLut_[depth][speed] = rgb565(
          static_cast<int>((red + (255 - red) * lift) * depthBrightness),
          static_cast<int>((green + (255 - green) * lift) * depthBrightness),
          static_cast<int>((blue + (255 - blue) * lift) * depthBrightness));
    }
  }
}

void FluidBox::publishRenderSnapshot() {
  int snapshotIndex = -1;

  portENTER_CRITICAL(&snapshotMux_);
  for (uint8_t i = 0; i < kSnapshotCount; ++i) {
    if (snapshotStates_[i] == SnapshotState::kFree) {
      snapshotIndex = i;
      break;
    }
  }

  // With one reader and one published frame, the third slot should always be
  // free. Reclaiming an unread published frame is still safe and keeps the
  // simulation non-blocking if task timing changes in the future.
  if (snapshotIndex < 0 && publishedSnapshot_ >= 0) {
    snapshotIndex = publishedSnapshot_;
    publishedSnapshot_ = -1;
  }
  if (snapshotIndex >= 0) {
    snapshotStates_[snapshotIndex] = SnapshotState::kWriting;
  }
  portEXIT_CRITICAL(&snapshotMux_);

  if (snapshotIndex < 0) {
    return;
  }

  RenderParticle* target = renderSnapshots_[snapshotIndex];
  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    target[i].position = particles_[i].position;
    target[i].speed = particles_[i].speed;
  }

  portENTER_CRITICAL(&snapshotMux_);
  if (publishedSnapshot_ >= 0) {
    snapshotStates_[publishedSnapshot_] = SnapshotState::kFree;
  }
  snapshotStates_[snapshotIndex] = SnapshotState::kPublished;
  publishedSnapshot_ = snapshotIndex;
  portEXIT_CRITICAL(&snapshotMux_);
}

int FluidBox::acquireRenderSnapshot() {
  int snapshotIndex = -1;
  portENTER_CRITICAL(&snapshotMux_);
  if (publishedSnapshot_ >= 0) {
    snapshotIndex = publishedSnapshot_;
    publishedSnapshot_ = -1;
    snapshotStates_[snapshotIndex] = SnapshotState::kReading;
  }
  portEXIT_CRITICAL(&snapshotMux_);
  return snapshotIndex;
}

void FluidBox::releaseRenderSnapshot(uint8_t snapshotIndex) {
  if (snapshotIndex >= kSnapshotCount) {
    return;
  }
  portENTER_CRITICAL(&snapshotMux_);
  if (snapshotStates_[snapshotIndex] == SnapshotState::kReading) {
    snapshotStates_[snapshotIndex] = SnapshotState::kFree;
  }
  portEXIT_CRITICAL(&snapshotMux_);
}

void FluidBox::prepareProjection(const RenderParticle* particles) {
  uint16_t depthCounts[cfg::kDepthLevels]{};
  uint16_t depthCursor[cfg::kDepthLevels]{};

  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    const RenderParticle& p = particles[i];
    const float scale = cfg::kProjectionFocal / (cfg::kProjectionFocal + p.position.z);
    ProjectedParticle& projected = projected_[i];
    projected.x = static_cast<int16_t>(centerX_ + (p.position.x - centerX_) * scale + 0.5f);
    projected.y = static_cast<int16_t>(centerY_ + (p.position.y - centerY_) * scale + 0.5f);
    projected.radius = static_cast<uint8_t>(clampInt(
        static_cast<int>(cfg::kParticleRadius * scale + 0.5f),
        1, cfg::kMaxDrawRadius));

    const int depthLevel = clampInt(static_cast<int>(
        p.position.z * (cfg::kDepthLevels - 1) / cfg::kBoxDepth + 0.5f),
        0, cfg::kDepthLevels - 1);
    const int speedLevel = clampInt(static_cast<int>(
        p.speed * (cfg::kSpeedLevels - 1) / cfg::kSpeedColorMax),
        0, cfg::kSpeedLevels - 1);
    projected.depthLevel = static_cast<uint8_t>(depthLevel);
    projected.color = colorLut_[depthLevel][speedLevel];
    projected.highlight = highlightLut_[depthLevel][speedLevel];
    ++depthCounts[depthLevel];
  }

  uint16_t offset = 0;
  for (int depth = 0; depth < cfg::kDepthLevels; ++depth) {
    depthCursor[depth] = offset;
    offset = static_cast<uint16_t>(offset + depthCounts[depth]);
  }

  for (size_t i = 0; i < cfg::kParticleCount; ++i) {
    const int depth = projected_[i].depthLevel;
    drawOrder_[depthCursor[depth]++] = static_cast<uint16_t>(i);
  }
}

FluidBox::RenderTiming FluidBox::render(LGFX_Device& display) {
  RenderTiming timing;
  const int snapshotIndex = acquireRenderSnapshot();
  if (snapshotIndex < 0) {
    const uint32_t clearStarted = fluidboxMicros();
    display.fillScreen(TFT_BLACK);
    timing.clearMicros = fluidboxMicros() - clearStarted;
    return timing;
  }

  timing.snapshotAvailable = true;
  uint32_t phaseStarted = fluidboxMicros();
  prepareProjection(renderSnapshots_[snapshotIndex]);
  timing.projectionMicros = fluidboxMicros() - phaseStarted;

  phaseStarted = fluidboxMicros();
  display.fillScreen(TFT_BLACK);
  timing.clearMicros = fluidboxMicros() - phaseStarted;

  // Counting sort above places near particles first. Reverse traversal paints
  // the far plane first so nearer particles naturally occlude it.
  phaseStarted = fluidboxMicros();
  for (int order = static_cast<int>(cfg::kParticleCount) - 1; order >= 0; --order) {
    const ProjectedParticle& p = projected_[drawOrder_[order]];
    display.fillCircle(p.x, p.y, p.radius, p.color);
    if (cfg::kHighlightsEnabled && p.radius >= 3) {
      const int highlightRadius = p.radius / 2;
      display.fillCircle(p.x - p.radius / 3,
                         p.y - p.radius / 3,
                         highlightRadius,
                         p.highlight);
    }
  }
  timing.particleMicros = fluidboxMicros() - phaseStarted;
  releaseRenderSnapshot(static_cast<uint8_t>(snapshotIndex));
  return timing;
}

// clang-format on
