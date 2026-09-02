#include "TextureGen.h"

#include "GenRandom.h"
#include "GeneratorSchema.h"
#include "PerlinNoise.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace gen {
namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Texel {
  glm::vec3 albedo{1.0f};
  float alpha = 1.0f;
  float roughness = 0.8f;
  float ao = 1.0f;
  // Unitless surface height used only to derive a tangent-space normal map.
  // Keeping it beside the other sampled properties guarantees every output
  // map comes from the same deterministic procedural evaluation.
  float height = 0.5f;
};

// A texture generator: a schema plus a per-pixel sampler over (u,v) in [0,1).
struct TexGen {
  std::string name;
  nlohmann::json schema;
  std::function<Texel(float u, float v, const Params &, const PerlinNoise &)> sample;
};

// --- noise helpers ---------------------------------------------------------

// Seamless in U by sampling the noise field around a circle: u=0 and u=1 land
// on the same point, so anything wrapped around a trunk or barrel has no
// visible seam. V is left linear -- the shapes that tile vertically (bark
// running up a long trunk) hide the seam in their own high-frequency detail,
// and making both axes seamless needs 4D noise this engine doesn't have.
float cylNoise(const PerlinNoise &n, float u, float v, float freqU, float freqV) {
  const float a = u * 2.0f * kPi;
  const float r = freqU / (2.0f * kPi);
  return n.noise(std::cos(a) * r + 13.0f, std::sin(a) * r + v * freqV + 7.0f);
}

float fbm(const PerlinNoise &n, float u, float v, float freqU, float freqV,
          int octaves, float gain = 0.5f) {
  float sum = 0.0f, amp = 1.0f, norm = 0.0f, f = 1.0f;
  for (int i = 0; i < octaves; ++i) {
    sum += cylNoise(n, u, v, freqU * f, freqV * f) * amp;
    norm += amp;
    amp *= gain;
    f *= 2.0f;
  }
  return norm > 0.0f ? sum / norm : 0.0f;
}

float ridged(const PerlinNoise &n, float u, float v, float fu, float fv, int oct) {
  return 1.0f - std::fabs(fbm(n, u, v, fu, fv, oct)) * 2.0f;
}

float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }

// Common parameters every texture generator accepts.
SchemaBuilder baseSchema(glm::vec3 defaultColor) {
  SchemaBuilder s;
  s.integer("resolution", 256, 32, 1024,
            "Texture size in pixels per side. 256 is plenty for most props; "
            "512+ only for surfaces filling the screen.")
      .color("color", defaultColor, "Dominant colour, linear RGB.")
      .number("contrast", 1.0f, 0.0f, 3.0f,
              "How strongly the pattern varies from the base colour.")
      .number("scale", 1.0f, 0.05f, 16.0f,
              "Pattern frequency multiplier. Higher is finer/busier.")
      .number("normalStrength", 1.0f, 0.0f, 3.0f,
              "Strength of the generated surface-relief normal map. Zero "
              "keeps the surface geometrically smooth.");
  return s;
}

// --- the generators --------------------------------------------------------

std::vector<TexGen> makeGenerators() {
  std::vector<TexGen> gens;

  { // bark: vertical ridges, deep fissures, fine grain
    TexGen g;
    g.name = "tex.bark";
    g.schema = baseSchema(glm::vec3(0.32f, 0.24f, 0.17f))
                   .number("ridgeDepth", 0.6f, 0.0f, 1.0f,
                           "Depth of the vertical fissures. Low is a smooth "
                           "birch, high is a deeply furrowed oak or pine.")
                   .number("ridgeScale", 10.0f, 1.0f, 40.0f,
                           "How many ridges run around the trunk.")
                   .schema();
    g.sample = [](float u, float v, const Params &p, const PerlinNoise &n) {
      const float s = p.num("scale", 1.0f);
      const float depth = p.num("ridgeDepth", 0.6f);
      const float ridgeScale = p.num("ridgeScale", 10.0f);
      // Stretched vertically: bark features run along the trunk, not around.
      const float ridge = ridged(n, u, v, ridgeScale * s, 1.5f * s, 3);
      const float grain = fbm(n, u, v, 40.0f * s, 12.0f * s, 2);
      const float h = saturate(ridge * 0.5f + 0.5f) * depth + grain * 0.15f;

      Texel t;
      const glm::vec3 base = p.color("color", glm::vec3(0.32f, 0.24f, 0.17f));
      const float k = (h - 0.4f) * p.num("contrast", 1.0f);
      t.albedo = glm::clamp(base * (1.0f + k * 0.9f), glm::vec3(0.0f), glm::vec3(1.0f));
      t.roughness = saturate(0.75f + h * 0.25f);
      t.height = h;
      // Fissures are self-shadowed; this is the only occlusion cue a flat
      // card or untessellated trunk gets.
      t.ao = saturate(0.55f + h * 0.65f);
      return t;
    };
    gens.push_back(std::move(g));
  }

  { // rock: mottled fbm with darker cracks
    TexGen g;
    g.name = "tex.rock";
    g.schema = baseSchema(glm::vec3(0.42f, 0.40f, 0.38f))
                   .number("crackDepth", 0.5f, 0.0f, 1.0f,
                           "Darkness of the crack network.")
                   .number("speckle", 0.25f, 0.0f, 1.0f,
                           "Fine mineral speckling.")
                   .schema();
    g.sample = [](float u, float v, const Params &p, const PerlinNoise &n) {
      const float s = p.num("scale", 1.0f);
      const float mottle = fbm(n, u, v, 6.0f * s, 6.0f * s, 4) * 0.5f + 0.5f;
      const float crackN = ridged(n, u, v, 14.0f * s, 14.0f * s, 2);
      // Ridged noise is bright over a surprisingly wide area.  The previous
      // low threshold turned most of the texture into outlined cells; only
      // retain the narrow ridge peaks so this reads as occasional fissures.
      const float crack = glm::smoothstep(0.80f, 0.94f, crackN);
      const float speck = fbm(n, u, v, 60.0f * s, 60.0f * s, 1) * 0.5f + 0.5f;

      Texel t;
      const glm::vec3 base = p.color("color", glm::vec3(0.42f, 0.40f, 0.38f));
      const float c = p.num("contrast", 1.0f);
      glm::vec3 a = base * (0.88f + (mottle - 0.5f) * 0.32f * c);
      a *= 1.0f - crack * p.num("crackDepth", 0.5f) * 0.48f;
      a += glm::vec3(speck - 0.5f) * p.num("speckle", 0.25f) * 0.10f;
      t.albedo = glm::clamp(a, glm::vec3(0.0f), glm::vec3(1.0f));
      t.roughness = saturate(0.7f + mottle * 0.3f);
      t.ao = saturate(1.0f - crack * 0.34f);
      t.height = saturate(0.40f + mottle * 0.34f - crack * 0.28f +
                          (speck - 0.5f) * 0.05f);
      return t;
    };
    gens.push_back(std::move(g));
  }

  { // leaf: a cutout leaf shape with a midrib and veins
    TexGen g;
    g.name = "tex.leaf";
    g.schema = baseSchema(glm::vec3(0.20f, 0.42f, 0.16f))
                   .number("autumn", 0.0f, 0.0f, 1.0f,
                           "Blends the green toward autumn orange/brown.")
                   .number("veinStrength", 0.35f, 0.0f, 1.0f,
                           "Visibility of the midrib and side veins.")
                   .number("width", 0.72f, 0.1f, 1.0f,
                           "How wide the leaf silhouette is within the card. "
                           "Lower values give a narrow needle-like blade.")
                   .boolean("cutout", true,
                            "Cut the card to a leaf silhouette using alpha. "
                            "Turn off for a full-card texture.")
                   .schema();
    g.sample = [](float u, float v, const Params &p, const PerlinNoise &n) {
      Texel t;
      const float width = p.num("width", 0.72f);
      // Leaf silhouette: a lens shape, widest at mid-height, tapering to a
      // point at both ends.
      const float halfW = width * 0.5f * std::sin(v * kPi) * (1.0f - 0.15f * v);
      const float dx = std::fabs(u - 0.5f);
      const float inside = halfW - dx;

      if (p.boolean("cutout", true))
        t.alpha = saturate(inside * 24.0f); // a few pixels of soft edge
      else
        t.alpha = 1.0f;

      const glm::vec3 green = p.color("color", glm::vec3(0.20f, 0.42f, 0.16f));
      const glm::vec3 autumnCol(0.62f, 0.31f, 0.08f);
      glm::vec3 base = glm::mix(green, autumnCol, p.num("autumn", 0.0f));

      // Midrib plus angled side veins.
      const float midrib = std::exp(-dx * dx * 900.0f);
      const float side = std::pow(saturate(std::sin((v * 14.0f + dx * 18.0f) * kPi)), 24.0f);
      const float vein = saturate(midrib + side * 0.5f) * p.num("veinStrength", 0.35f);
      base = glm::mix(base, base * 1.5f + glm::vec3(0.05f), vein);

      // Slight blotchiness so a canopy is not one flat colour.
      const float blotch = fbm(n, u, v, 5.0f, 5.0f, 2) * 0.5f + 0.5f;
      base *= 0.88f + blotch * 0.24f * p.num("contrast", 1.0f);

      t.albedo = glm::clamp(base, glm::vec3(0.0f), glm::vec3(1.0f));
      t.roughness = 0.65f;
      t.height = saturate(0.38f + vein * 0.48f + (blotch - 0.5f) * 0.10f);
      // Slightly darker toward the base, where leaves shade each other. Kept
      // shallow on purpose: foliage cards are usually seen against the sky or
      // facing away from the sun, and a strong gradient on top of that reads
      // as a dying tree rather than a shaded one.
      t.ao = saturate(0.78f + v * 0.22f);
      return t;
    };
    gens.push_back(std::move(g));
  }

  { // grass blade: vertical gradient, cutout taper
    TexGen g;
    g.name = "tex.grass";
    g.schema = baseSchema(glm::vec3(0.26f, 0.44f, 0.16f))
                   .color("tipColor", glm::vec3(0.48f, 0.55f, 0.22f),
                          "Colour at the blade tips.")
                   .number("dryness", 0.0f, 0.0f, 1.0f,
                           "Blends toward straw yellow.")
                   .boolean("cutout", true,
                            "Taper the card to a blade silhouette with alpha.")
                   .schema();
    g.sample = [](float u, float v, const Params &p, const PerlinNoise &n) {
      Texel t;
      const float halfW = 0.5f * (1.0f - v * 0.85f);
      const float dx = std::fabs(u - 0.5f);
      t.alpha = p.boolean("cutout", true) ? saturate((halfW - dx) * 30.0f) : 1.0f;

      const glm::vec3 root = p.color("color", glm::vec3(0.26f, 0.44f, 0.16f));
      const glm::vec3 tip = p.color("tipColor", glm::vec3(0.48f, 0.55f, 0.22f));
      glm::vec3 base = glm::mix(root, tip, v * v);
      base = glm::mix(base, glm::vec3(0.68f, 0.60f, 0.30f), p.num("dryness", 0.0f));
      const float streak = fbm(n, u, v, 2.0f, 20.0f, 2) * 0.5f + 0.5f;
      base *= 0.9f + streak * 0.2f * p.num("contrast", 1.0f);

      t.albedo = glm::clamp(base, glm::vec3(0.0f), glm::vec3(1.0f));
      t.roughness = 0.8f;
      t.ao = saturate(0.45f + v * 0.55f); // dark at the root, lit at the tip
      t.height = saturate(0.45f + (streak - 0.5f) * 0.22f - dx * 0.08f);
      return t;
    };
    gens.push_back(std::move(g));
  }

  { // planed wood: growth rings + grain, for crates and planks
    TexGen g;
    g.name = "tex.wood";
    g.schema = baseSchema(glm::vec3(0.55f, 0.40f, 0.24f))
                   .number("ringScale", 9.0f, 1.0f, 40.0f,
                           "Growth rings across the board.")
                   .number("grain", 0.4f, 0.0f, 1.0f,
                           "Fine grain streaking along the board.")
                   .schema();
    g.sample = [](float u, float v, const Params &p, const PerlinNoise &n) {
      const float s = p.num("scale", 1.0f);
      // Warp the ring coordinate so rings wander like real timber instead of
      // reading as printed stripes.
      const float warp = fbm(n, u, v, 3.0f * s, 1.0f * s, 3) * 0.35f;
      const float rings =
          std::sin((u + warp) * p.num("ringScale", 9.0f) * s * kPi) * 0.5f + 0.5f;
      const float grain = fbm(n, u, v, 2.0f * s, 90.0f * s, 2) * 0.5f + 0.5f;

      Texel t;
      const glm::vec3 base = p.color("color", glm::vec3(0.55f, 0.40f, 0.24f));
      const float c = p.num("contrast", 1.0f);
      const float k = rings * 0.35f + grain * p.num("grain", 0.4f) * 0.3f;
      t.albedo = glm::clamp(base * (0.82f + k * 0.5f * c), glm::vec3(0.0f),
                            glm::vec3(1.0f));
      t.roughness = saturate(0.6f + rings * 0.25f);
      t.ao = 1.0f;
      t.height = saturate(0.35f + rings * 0.38f +
                          (grain - 0.5f) * p.num("grain", 0.4f) * 0.22f);
      return t;
    };
    gens.push_back(std::move(g));
  }

  { // metal: brushed streaks, low roughness, scratches
    TexGen g;
    g.name = "tex.metal";
    g.schema = baseSchema(glm::vec3(0.56f, 0.57f, 0.60f))
                   .number("polish", 0.6f, 0.0f, 1.0f,
                           "How smooth the surface is. High is a mirror, low "
                           "is cast or heavily brushed.")
                   .number("rust", 0.0f, 0.0f, 1.0f,
                           "Patches of orange corrosion, which also roughen "
                           "the surface where they appear.")
                   .schema();
    g.sample = [](float u, float v, const Params &p, const PerlinNoise &n) {
      const float s = p.num("scale", 1.0f);
      const float brush = fbm(n, u, v, 120.0f * s, 2.0f * s, 2) * 0.5f + 0.5f;
      const float rustMask =
          saturate((fbm(n, u, v, 4.0f * s, 4.0f * s, 3) * 0.5f + 0.5f - 0.45f) *
                   3.0f) * p.num("rust", 0.0f);

      Texel t;
      const glm::vec3 base = p.color("color", glm::vec3(0.56f, 0.57f, 0.60f));
      glm::vec3 a = base * (0.9f + brush * 0.2f * p.num("contrast", 1.0f));
      a = glm::mix(a, glm::vec3(0.42f, 0.20f, 0.08f), rustMask);
      t.albedo = glm::clamp(a, glm::vec3(0.0f), glm::vec3(1.0f));
      const float polish = p.num("polish", 0.6f);
      t.roughness = saturate(glm::mix(0.55f, 0.12f, polish) + brush * 0.1f +
                             rustMask * 0.6f);
      t.ao = 1.0f;
      t.height = saturate(0.48f + (brush - 0.5f) * 0.10f + rustMask * 0.22f);
      return t;
    };
    gens.push_back(std::move(g));
  }

  return gens;
}

const std::vector<TexGen> &generators() {
  static const std::vector<TexGen> gens = makeGenerators();
  return gens;
}

const TexGen *findGen(const std::string &name) {
  for (const auto &g : generators())
    if (g.name == name)
      return &g;
  return nullptr;
}

MeshImage makeImage(const std::string &key, int size, int components) {
  MeshImage img;
  img.key = key;
  img.width = size;
  img.height = size;
  img.component = components;
  img.pixels.resize(static_cast<size_t>(size) * size * components);
  return img;
}

uint8_t toByte(float v) {
  return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

} // namespace

std::vector<std::string> textureGeneratorNames() {
  std::vector<std::string> out;
  for (const auto &g : generators())
    out.push_back(g.name);
  return out;
}

const nlohmann::json *textureGeneratorSchema(const std::string &name) {
  const TexGen *g = findGen(name);
  return g ? &g->schema : nullptr;
}

bool generateTextureSet(const std::string &generator, nlohmann::json params,
                        uint32_t seed, const std::string &keyPrefix,
                        TextureSet &out, std::vector<std::string> &warnings,
                        std::string &error) {
  const TexGen *g = findGen(generator);
  if (!g) {
    error = "unknown texture generator '" + generator + "'";
    return false;
  }
  if (!validateParams(g->schema, params, warnings, error))
    return false;

  const Params p(params);
  const int size = p.integer("resolution", 256);
  const PerlinNoise noise(seed);

  MeshImage albedo = makeImage(keyPrefix + "/albedo", size, 4);
  MeshImage rough = makeImage(keyPrefix + "/roughness", size, 1);
  MeshImage ao = makeImage(keyPrefix + "/ao", size, 1);
  MeshImage normal = makeImage(keyPrefix + "/normal", size, 4);
  std::vector<float> heights(static_cast<size_t>(size) * size, 0.5f);
  bool anyAO = false;

  for (int y = 0; y < size; ++y) {
    // v=0 at the bottom of the image. Model UVs are raw (OBJ bottom-left
    // origin) and the renderer does NOT flip embedded payloads the way it
    // flips files, so generating bottom-up here is what makes a leaf's tip
    // land at the card's tip.
    const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
    for (int x = 0; x < size; ++x) {
      const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
      const Texel t = g->sample(u, v, p, noise);
      const size_t i = static_cast<size_t>(y) * size + x;
      albedo.pixels[i * 4 + 0] = toByte(t.albedo.r);
      albedo.pixels[i * 4 + 1] = toByte(t.albedo.g);
      albedo.pixels[i * 4 + 2] = toByte(t.albedo.b);
      albedo.pixels[i * 4 + 3] = toByte(t.alpha);
      rough.pixels[i] = toByte(t.roughness);
      ao.pixels[i] = toByte(t.ao);
      heights[i] = t.height;
      if (t.ao < 0.999f)
        anyAO = true;
    }
  }

  // Central differences over the deterministic height field. U wraps because
  // the procedural samplers are cylindrical/seamless there; V clamps so a
  // leaf tip or board edge never reads from the opposite side of the image.
  const float normalStrength = p.num("normalStrength", 1.0f);
  if (normalStrength > 0.0001f) {
    auto heightAt = [&](int x, int y) {
      x = (x % size + size) % size;
      y = std::clamp(y, 0, size - 1);
      return heights[static_cast<size_t>(y) * size + x];
    };
    const float slope = normalStrength * static_cast<float>(size) * 0.035f;
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        const float dx = (heightAt(x + 1, y) - heightAt(x - 1, y)) * 0.5f;
        const float dy = (heightAt(x, y + 1) - heightAt(x, y - 1)) * 0.5f;
        const glm::vec3 n =
            glm::normalize(glm::vec3(-dx * slope, -dy * slope, 1.0f));
        const size_t i = static_cast<size_t>(y) * size + x;
        normal.pixels[i * 4 + 0] = toByte(n.x * 0.5f + 0.5f);
        normal.pixels[i * 4 + 1] = toByte(n.y * 0.5f + 0.5f);
        normal.pixels[i * 4 + 2] = toByte(n.z * 0.5f + 0.5f);
        normal.pixels[i * 4 + 3] = 255;
      }
    }
  }

  out = TextureSet{};
  out.albedoKey = albedo.key;
  out.roughnessKey = rough.key;
  out.images.push_back(std::move(albedo));
  out.images.push_back(std::move(rough));
  if (normalStrength > 0.0001f) {
    out.normalKey = normal.key;
    out.images.push_back(std::move(normal));
  }
  if (anyAO) {
    out.aoKey = ao.key;
    out.images.push_back(std::move(ao));
  }
  return true;
}

void applyTextureSet(MeshData &mesh, MaterialAsset &material, TextureSet &&set) {
  if (!set.valid())
    return;
  material.texDiffusePath = set.albedoKey;
  material.texRoughnessPath = set.roughnessKey;
  // Single-channel payloads: the renderer expands them to RGB, so any channel
  // selector reads the same value. Left at the default R.
  material.roughnessChannel = 0;
  if (!set.normalKey.empty())
    material.texNormalPath = set.normalKey;
  if (!set.aoKey.empty()) {
    material.texAOPath = set.aoKey;
    material.aoChannel = 0;
  }
  for (auto &img : set.images)
    mesh.images.push_back(std::move(img));
  set.images.clear();
}

} // namespace gen
