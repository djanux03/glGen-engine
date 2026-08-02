#include "TerrainNoise.h"

#include <algorithm>
#include <cmath>

namespace {

float saturate01(float v) { return std::clamp(v, 0.0f, 1.0f); }

float smooth01(float a, float b, float v) {
  float t = saturate01((v - a) / std::max(0.0001f, b - a));
  return t * t * (3.0f - 2.0f * t);
}

// Derivative-damped ("eroded") fbm (plan §4b.3): each octave's amplitude is
// reduced where the accumulated gradient from previous octaves is already
// steep, as if loose material had already slid off the steep parts --
// produces smooth sediment-like hollows and sharper convex breaks without a
// real hydraulic-erosion simulation. `erosionStrength` 0 reduces this to
// plain fbm (verified: damp = 1/(1+0*|derivSum|) = 1 always).
float erodedFbm(const PerlinNoise &n, float x, float z, int octaves,
                float lacunarity, float gain, float erosionStrength) {
  float amplitude = 1.0f;
  float frequency = 1.0f;
  float value = 0.0f;
  glm::vec2 derivSum(0.0f);
  const float eps = 0.35f; // gradient-estimate offset, in noise-space units

  for (int i = 0; i < octaves; ++i) {
    const float fx = x * frequency, fz = z * frequency;
    const float h = n.noise(fx, fz);
    const float hx = n.noise(fx + eps, fz);
    const float hz = n.noise(fx, fz + eps);
    const glm::vec2 grad((hx - h) / eps, (hz - h) / eps);

    const float damp = 1.0f / (1.0f + std::max(0.0f, erosionStrength) *
                                          glm::length(derivSum));
    value += h * amplitude * damp;
    derivSum += grad * amplitude;

    amplitude *= gain;
    frequency *= lacunarity;
  }
  return value;
}

} // namespace

TerrainMacroSample sampleMacro(const TerrainNoiseSet &n, glm::vec2 worldXZ,
                                const TerrainSettings &settings) {
  const float worldX = worldXZ.x;
  const float worldZ = worldXZ.y;

  TerrainMacroSample s;

  // Mountain-region wavelength: ~2.8km base, narrower with higher
  // mountainRegionScale (more, smaller mountain regions) or wider with
  // lower (fewer, bigger ones).
  const float regionFreq =
      std::max(0.00003f, 0.00035f / std::max(0.1f, settings.mountainRegionScale));
  const float terrainFreq = std::max(0.0001f, settings.noiseFrequency);

  // --- domain warp (dedicated `warp` instance, decorrelated by offset) ---
  const float warpFreq = regionFreq * 1.3f;
  const float warpAmp = settings.chunkWorldSize * 1.75f;
  const float warpX = n.warp.noise(worldX * warpFreq + 31.7f, worldZ * warpFreq - 14.2f);
  const float warpZ = n.warp.noise(worldX * warpFreq - 53.1f, worldZ * warpFreq + 22.8f);
  s.warpedX = worldX + warpX * warpAmp;
  s.warpedZ = worldZ + warpZ * warpAmp;

  // --- moisture (feeds forest weight + later turf lushness) ---
  const float climateFreq = regionFreq * 1.6f;
  s.moisture = n.moistureNoise.fbm(s.warpedX * climateFreq - 140.0f,
                                   s.warpedZ * climateFreq + 80.0f, 3, 2.0f, 0.5f) *
                   0.5f + 0.5f;

  // --- mountain-region macro mask: coverage-gated on plain fbm (NOT
  // ridgeNoise -- ridgeNoise's `1-|n|` then squared per octave skews its
  // output distribution heavily toward 1, so naively thresholding it around
  // 0.5-0.9 made ~89% of the world qualify as mountain at mountainCoverage
  // =0.32, verified via a wide-area probe before this fix). fbm's
  // well-behaved, roughly-symmetric distribution around 0 (remapped 0..1
  // below) is what continentalness/moisture used successfully in the old
  // system, so coverage now actually tracks mountainCoverage. The "ranges
  // read as connected chains" ridged character comes from macroRidge below
  // (an explicit relief-sharpness term, not this coverage gate). ---
  const float mountainA =
      n.base.fbm(s.warpedX * regionFreq + 230.0f, s.warpedZ * regionFreq - 170.0f,
                4, 2.0f, 0.5f) * 0.5f + 0.5f;
  const float mountainB =
      n.moistureNoise.fbm(s.warpedX * regionFreq * 0.55f - 900.0f,
                          s.warpedZ * regionFreq * 0.55f + 300.0f, 3, 2.0f, 0.5f) *
          0.5f + 0.5f;
  const float coverage = saturate01(settings.mountainCoverage);
  const float mountainStart = 0.62f - coverage * 0.55f;
  const float mountainEnd = mountainStart + 0.22f;
  s.macroMountain = smooth01(mountainStart, mountainEnd, mountainA * 0.8f + mountainB * 0.2f);

  // --- valley driver: broad low-freq field, low values = valley floor ---
  const float valleyFreq = regionFreq * 0.9f;
  const float valleyField =
      n.base.fbm(s.warpedX * valleyFreq + 1240.0f, s.warpedZ * valleyFreq - 860.0f, 3, 2.0f, 0.5f) *
          0.5f + 0.5f;
  const float valleySpan = std::max(0.35f, settings.valleySpan);
  s.macroValley = 1.0f - smooth01(0.16f, 0.44f + (valleySpan - 1.0f) * 0.18f, valleyField);
  s.macroRidge = n.base.ridgeNoise(s.warpedX * valleyFreq * 1.15f - 250.0f,
                                   s.warpedZ * valleyFreq * 1.15f + 510.0f, 3, 2.0f, 0.5f) *
                 (0.45f + 0.55f * s.macroMountain);

  // --- base terrain shape: lowland rolling + eroded detail + mountain ridges ---
  s.broadShape = n.base.fbm(s.warpedX * terrainFreq * 0.18f - 320.0f,
                            s.warpedZ * terrainFreq * 0.18f + 190.0f, 3, 2.0f, 0.5f);
  s.baseShape = erodedFbm(n.base, s.warpedX * terrainFreq * 0.65f,
                          s.warpedZ * terrainFreq * 0.65f, settings.octaves,
                          settings.lacunarity, settings.gain, settings.erosionStrength);
  s.ridgeShape = settings.useRidgeNoise
                     ? n.base.ridgeNoise(s.warpedX * terrainFreq * 0.80f,
                                        s.warpedZ * terrainFreq * 0.80f, settings.octaves,
                                        settings.lacunarity, settings.gain)
                     : std::abs(n.base.fbm(s.warpedX * terrainFreq * 0.80f,
                                           s.warpedZ * terrainFreq * 0.80f, settings.octaves,
                                           settings.lacunarity, settings.gain));

  // --- forest coverage field: independent ~400m-wavelength noise, gated
  // later by moisture/mountain/treeline in sampleBiomeWeights() ---
  const float forestFreq = 1.0f / 400.0f;
  s.forestField = n.forestNoise.fbm(worldX * forestFreq + 77.0f, worldZ * forestFreq - 233.0f,
                                    3, 2.0f, 0.5f) *
                      0.5f + 0.5f;

  // --- rock-outcrop hint: higher-frequency ridge noise for organic edges,
  // combined with real slope by the mesher (not slope-thresholded here) ---
  const float outcropFreq = terrainFreq * 2.2f;
  s.outcropField = n.detail.ridgeNoise(s.warpedX * outcropFreq + 410.0f,
                                       s.warpedZ * outcropFreq - 90.0f, 3, 2.1f, 0.5f);

  return s;
}

float computeHeight(const TerrainMacroSample &sample, const TerrainNoiseSet &n,
                    glm::vec2 worldXZ, const TerrainSettings &settings,
                    const HeightOffsetGrid *edits) {
  const float hs = settings.heightScale;
  const float macroStrength = std::max(0.0f, settings.macroStrength);

  const float macroMountain = std::pow(saturate01(sample.macroMountain), 0.85f);
  const float macroValley = std::pow(saturate01(sample.macroValley), 1.10f);
  const float macroRidge = sample.macroRidge * macroMountain;

  // Lowland rolling shape vs. mountain relief (eroded detail amplified by
  // real ridge noise), blended by the mountain-region driver -- one
  // landform system, not a biome switch (plan §4b.1: "Mountains/Valleys is
  // one landform system, not two").
  const float rolling = sample.broadShape * 0.65f + sample.baseShape * 0.35f;
  const float mountainRelief =
      sample.baseShape * 0.5f + sample.ridgeShape * (1.2f + macroMountain * 1.4f);
  const float shapeBlend = rolling * (1.0f - macroMountain) + mountainRelief * macroMountain;

  const float mountainHeightScale = std::max(0.0f, settings.mountainHeightScale);
  const float valleyDepth = std::max(0.0f, settings.valleyDepth);
  const float macroShape = macroMountain * 1.6f * mountainHeightScale +
                           macroRidge * 0.6f * mountainHeightScale -
                           macroValley * 0.95f * valleyDepth;

  float h = shapeBlend * hs * (0.45f + macroMountain * 0.35f) + macroShape * hs * macroStrength;

  // Micro-relief (0.3-1m bumps/hollows): calmer in valley floors, where real
  // drainage bottoms are flatter than the surrounding slopes.
  const float freq = std::max(0.0001f, settings.noiseFrequency);
  const float micro =
      n.detail.noise(sample.warpedX * freq * 4.0f + 150.0f, sample.warpedZ * freq * 4.0f - 70.0f) *
      0.035f;
  const float fine =
      n.detail.noise(sample.warpedX * freq * 8.0f - 320.0f, sample.warpedZ * freq * 8.0f + 260.0f) *
      0.012f;
  const float microStrength =
      std::max(0.0f, settings.microReliefStrength) * (1.0f - macroValley * 0.6f);
  h += (micro + fine) * hs * microStrength;

  if (edits) {
    h += edits->sampleBilinear(worldXZ, settings.chunkWorldSize, settings.chunkResolution);
  }

  return h;
}

BiomeWeights sampleBiomeWeights(const TerrainMacroSample &sample,
                                const TerrainSettings &settings, float height) {
  // Mountain weight: the region driver, softened on valley floors inside
  // mountain country so they shade/scatter like lowland (plan §4b': "a
  // mountain-region valley floor has low wMountain").
  const float wMountain = saturate01(sample.macroMountain) *
                          (1.0f - saturate01(sample.macroValley) * 0.6f);

  // Forest weight: coverage-gated noise field x moisture bias x treeline
  // altitude cutoff, excluded from mountain country.
  const float coverage = saturate01(settings.forestCoverage);
  const float forestThreshold = 1.0f - coverage;
  const float forestSignal = sample.forestField * 0.7f + sample.moisture * 0.3f;
  const float forestMask =
      smooth01(forestThreshold - 0.18f, forestThreshold + 0.18f, forestSignal);
  const float transition = std::max(1.0f, settings.treelineTransition);
  const float treeline =
      1.0f - smooth01(settings.treelineHeight, settings.treelineHeight + transition, height);
  float wForest = forestMask * (1.0f - wMountain) * treeline;

  float wMeadow = 1.0f - wForest - wMountain;
  if (wMeadow < 0.0f) {
    // Forest+mountain overshot 1 in a transition zone -- renormalize rather
    // than let a consumer see weights that don't sum to 1.
    const float total = wForest + wMountain;
    wForest /= total;
    wMeadow = 0.0f;
  }

  BiomeWeights w;
  w.meadow = wMeadow;
  w.forest = wForest;
  w.mountain = wMountain;
  return w;
}

void sampleHeightGrid(const TerrainNoiseSet &n, const TerrainSettings &settings,
                      glm::vec2 chunkOrigin, float chunkWorldSize, uint32_t samplesPerEdge,
                      const HeightOffsetGrid *edits, std::vector<float> &outHeights,
                      std::vector<TerrainGroundFields> *outFields) {
  const uint32_t count = samplesPerEdge * samplesPerEdge;
  outHeights.clear();
  outHeights.reserve(count);
  if (outFields) {
    outFields->clear();
    outFields->reserve(count);
  }

  const float denom = (samplesPerEdge > 1) ? static_cast<float>(samplesPerEdge - 1) : 1.0f;

  for (uint32_t j = 0; j < samplesPerEdge; ++j) {
    for (uint32_t i = 0; i < samplesPerEdge; ++i) {
      glm::vec2 worldXZ = chunkOrigin + glm::vec2(static_cast<float>(i) / denom,
                                                  static_cast<float>(j) / denom) *
                                            chunkWorldSize;
      TerrainMacroSample sample = sampleMacro(n, worldXZ, settings);
      const float h = computeHeight(sample, n, worldXZ, settings, edits);
      outHeights.push_back(h);
      if (outFields) {
        TerrainGroundFields f;
        f.moisture = sample.moisture;
        const BiomeWeights bw = sampleBiomeWeights(sample, settings, h);
        f.wMeadow = bw.meadow;
        f.wForest = bw.forest;
        f.wMountain = bw.mountain;
        f.rockNoise = saturate01(sample.outcropField * 0.5f + 0.5f);
        outFields->push_back(f);
      }
    }
  }
}
