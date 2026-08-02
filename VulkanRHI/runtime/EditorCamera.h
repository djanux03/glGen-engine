#pragma once

// Free-fly editor viewport camera -- mouse-only navigation with smoothed,
// distance-adaptive motion. Replaces the old inline nav code in main.cpp:
//
//   RMB drag  = look around
//   MMB drag  = pan (screen-space, world units scale with height)
//   Scroll    = dolly zoom with momentum (glides, then eases out)
//
// There is deliberately NO keyboard movement here: in editor mode the
// keyboard belongs to the UI (W/E/R gizmo, Delete, Ctrl+S); only play mode
// moves anything with WASD. That is the editor/play mode split.
//
// "Adaptive" speed: zoom impulses and pan deltas scale with the camera's
// height above the terrain, so one scroll notch travels ~25% of your
// altitude -- flick the wheel from 200m up and you cross the map, roll it
// at eye level and you creep centimeters. Near the ground the approach is
// asymptotic (Google-Earth style), so you never blast through the surface.
//
// Input writes *targets*; update() exponentially smooths the rendered pose
// toward them and integrates the zoom momentum. All rates are exponential
// (frame-rate independent).

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

struct EditorCameraSettings {
  float lookSensitivity = 0.12f; // degrees per pixel of RMB drag
  float zoomSpeed = 1.0f;        // multiplier on scroll dolly distance
  float panSpeed = 1.0f;         // multiplier on MMB pan distance
  // 0 = instant/raw response, 1 = heavy glide. Applies to look and move.
  float smoothing = 0.45f;
  bool invertZoom = false;
  // Scale zoom/pan with height above terrain (off = fixed ~12 m reference).
  bool adaptiveSpeed = true;
};

class EditorCamera {
public:
  EditorCameraSettings settings;

  // Snap targets AND smoothed state to a pose (startup, or whenever
  // something else -- play mode, smoke-test env overrides -- moved the
  // camera out from under us).
  void seed(const glm::vec3 &pos, float yawDeg, float pitchDeg) {
    mPosTarget = pos;
    mYawTarget = yawDeg;
    mPitchTarget = pitchDeg;
    mZoomVel = 0.0f;
  }

  void addLook(float dxPixels, float dyPixels) {
    mYawTarget -= dxPixels * settings.lookSensitivity;
    mPitchTarget = std::clamp(mPitchTarget - dyPixels * settings.lookSensitivity,
                              -89.0f, 89.0f);
  }

  // groundY: terrain height under the camera (for the adaptive scale).
  void addPan(float dxPixels, float dyPixels, float groundY) {
    const glm::vec3 fwd = forwardFromTargets();
    const glm::vec3 right =
        glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up = glm::cross(right, fwd);
    const float perPixel = 0.0016f * speedScale(groundY) * settings.panSpeed;
    mPosTarget -= right * (dxPixels * perPixel);
    mPosTarget += up * (dyPixels * perPixel);
  }

  void addZoom(float notches, float groundY) {
    if (settings.invertZoom)
      notches = -notches;
    // Impulse in m/s; with the exp(-kZoomDecay t) falloff below, the total
    // glide distance per notch is impulse / kZoomDecay = ~25% of altitude.
    mZoomVel += notches * speedScale(groundY) * settings.zoomSpeed;
  }

  // Smooths pos/yaw/pitch toward the targets and writes them out.
  void update(float dt, glm::vec3 &pos, float &yawDeg, float &pitchDeg) {
    dt = std::max(dt, 1e-5f);

    // Zoom momentum rides along the (smoothed-target) view direction.
    if (std::abs(mZoomVel) > 1e-4f) {
      mPosTarget += forwardFromTargets() * (mZoomVel * dt);
      mZoomVel *= std::exp(-kZoomDecay * dt);
    } else {
      mZoomVel = 0.0f;
    }

    const float s = std::clamp(settings.smoothing, 0.0f, 1.0f);
    // Response rates in 1/s -- look stays snappier than translation so
    // mouse-look never feels rubbery, translation gets the glide.
    const float lookRate = lerp(60.0f, 14.0f, s);
    const float moveRate = lerp(40.0f, 6.0f, s);
    const float lookA = 1.0f - std::exp(-lookRate * dt);
    const float moveA = 1.0f - std::exp(-moveRate * dt);

    pos += (mPosTarget - pos) * moveA;
    yawDeg += (mYawTarget - yawDeg) * lookA;
    pitchDeg += (mPitchTarget - pitchDeg) * lookA;
  }

  const glm::vec3 &positionTarget() const { return mPosTarget; }

private:
  static constexpr float kZoomDecay = 4.0f; // 1/s momentum falloff

  static float lerp(float a, float b, float t) { return a + (b - a) * t; }

  // World-units-per-input reference distance. Adaptive: your height above
  // the ground (clamped so it never hits zero at the surface nor explodes
  // in orbit); fixed mode: a constant 12 m feel.
  float speedScale(float groundY) const {
    if (!settings.adaptiveSpeed)
      return 12.0f;
    return std::clamp(mPosTarget.y - groundY, 1.0f, 400.0f);
  }

  glm::vec3 forwardFromTargets() const {
    const float yaw = glm::radians(mYawTarget);
    const float pitch = glm::radians(mPitchTarget);
    return glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw),
                                    std::sin(pitch),
                                    std::cos(pitch) * std::cos(yaw)));
  }

  glm::vec3 mPosTarget{0.0f};
  float mYawTarget = 0.0f;
  float mPitchTarget = 0.0f;
  float mZoomVel = 0.0f; // m/s along view direction (scroll momentum)
};
