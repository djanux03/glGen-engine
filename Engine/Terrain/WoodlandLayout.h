#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/glm.hpp>

// Authored world-space landforms, shared by rendering, physics and scatter.
// The seed may vary individual plants, but never moves the pond or marsh.
namespace woodland {
inline float smooth(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
inline float ellipse(glm::vec2 p, glm::vec2 centre, glm::vec2 radius) {
  return glm::length((p - centre) / radius);
}
// The service loop and its shoreline spur are authored coordinates, not noise.
// Their coverage drives BOTH surface splats and vegetation exclusions.
inline const std::array<glm::vec2, 14> serviceTrack = {{
  {-330,90}, {-170,45}, {-70,18}, {0,12}, {65,6}, {122,-28},
  {145,-94}, {115,-165}, {10,-180}, {-90,-163}, {-202,-157},
  {-255,-70}, {-260,20}, {-330,90}}};
inline float segmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
  const glm::vec2 ab = b-a;
  return glm::length(p-a-ab*std::clamp(glm::dot(p-a,ab)/glm::dot(ab,ab),0.0f,1.0f));
}
inline float trackDistance(glm::vec2 p) {
  float d = 1e6f;
  for (size_t i=1; i<serviceTrack.size(); ++i)
    d = std::min(d, segmentDistance(p,serviceTrack[i-1],serviceTrack[i]));
  return d;
}
inline float trackCoverage(glm::vec2 p, float distance) {
  // The road centre stays walkable, but its shoulders erode at several scales.
  // A constant distance contour made a ruler-straight gravel/grass boundary.
  const float erosion = .65f*std::sin(p.x*.41f+p.y*.27f)
                      + .38f*std::sin(p.y*1.31f-p.x*.77f)
                      + .22f*std::sin(p.x*2.17f+p.y*1.73f);
  const float edge = distance + erosion*smooth(1.6f,3.2f,distance);
  return 1.0f-smooth(2.1f,5.6f,edge);
}
struct Sample { float height, forest, moisture, track, rock; };
inline Sample sample(glm::vec2 p, float worldRadius) {
  // A broad central pond opens toward the southern lookout. Scalloped banks
  // and a western drainage channel connect it to the shallow reed marsh.
  const float shore = 1.0f + 0.055f * std::sin(p.x * .065f + p.y * .027f)
                           + 0.035f * std::sin(p.y * .11f);
  const float pond = ellipse(p, {0, -80}, {82, 58}) / shore;
  const float marsh = ellipse(p, {-145, -95}, {61, 43}) / shore;
  const float channel = ellipse(p, {-78, -94}, {72, 13});
  const float northPool = ellipse(p, {58, -205}, {44, 31}) / shore;
  const float drainage = ellipse(p, {39, -153}, {15, 47});
  const float bank = std::min({pond, marsh, channel, northPool, drainage});

  // Positive banks and negative basin floors share one continuous surface;
  // the existing water plane at y=0 fills only the authored depressions.
  float h = -2.4f + 5.2f * smooth(.38f, 1.30f, bank);
  const float dry = smooth(1.05f, 1.85f, bank);
  h += dry * (1.3f + 1.1f * std::sin(p.x * .018f) * std::cos(p.y * .014f));
  h += dry * 6.0f * std::exp(-std::pow(ellipse(p, {170, -100}, {130, 160}), 2.0f));
  const float rockRise = std::max({
      1.0f-smooth(.15f,1.0f,ellipse(p,{-40,68},{25,20})),
      1.0f-smooth(.15f,1.0f,ellipse(p,{135,-8},{30,20})),
      1.0f-smooth(.15f,1.0f,ellipse(p,{-220,-180},{24,24}))});
  h += dry * 5.5f * rockRise;
  // Gentle ground irregularity remains beneath the textures. Centimetre-scale
  // wheel ruts do not turn the walking surface into noisy miniature mountains.
  h += dry * (.18f*std::sin(p.x*.21f)*std::sin(p.y*.17f)
             +.07f*std::sin(p.x*.49f+p.y*.38f));
  const float trackD = trackDistance(p);
  const float track = trackCoverage(p,trackD);
  h -= dry * (.12f*track + .13f*(1.0f-smooth(.15f,.65f,std::abs(trackD-1.1f))));
  // Low hummocks in the marsh leave islands among the connected shallows.
  h += 2.8f * (1.0f - smooth(.25f, 1.0f, ellipse(p, {-150, -91}, {13, 10})));
  h += 2.6f * (1.0f - smooth(.25f, 1.0f, ellipse(p, {-119, -112}, {11, 8})));
  const float rim = smooth(std::max(300.0f, worldRadius) - 140.0f,
                           std::max(300.0f, worldRadius), glm::length(p));
  h = glm::mix(h, -8.0f, rim);

  // Deliberate meadow clearings frame the pond; the higher eastern and
  // northern banks carry continuous woodland rather than noise-picked biomes.
  const float lookout = 1.0f - smooth(.35f, 1.0f, ellipse(p, {0, 12}, {72, 55}));
  const float meadow = 1.0f - smooth(.3f, 1.0f, ellipse(p, {98, 32}, {70, 55}));
  const float marshOpen = 1.0f - smooth(.7f, 1.45f, marsh);
  const float westClearing = 1.0f-smooth(.3f,1.0f,ellipse(p,{-240,40},{67,52}));
  const float northClearing = 1.0f-smooth(.3f,1.0f,ellipse(p,{-60,-225},{67,43}));
  const float grove = .65f + .2f*std::sin(p.x*.024f+p.y*.013f)
                           *std::cos(p.y*.031f-p.x*.009f);
  const float utilityClearing = 1.0f-smooth(.5f,1.0f,ellipse(p,{40,45},{210,18}));
  const float open = std::max({lookout, meadow, westClearing, northClearing, utilityClearing});
  const float forest = std::clamp(grove * (1.0f-.96f*open)
      *(1.0f-.7f*marshOpen)*(1.0f-track)*(1.0f-.8f*rockRise), .0f, .85f);
  // Damp banks transition to dry forest floor; the old nearly constant .65-1
  // moisture prevented dry dirt and wet shore materials from separating.
  const float moisture = .32f+.68f*(1.0f-smooth(.9f,1.7f,bank));
  return {h, forest, moisture, track, rockRise};
}
} // namespace woodland
