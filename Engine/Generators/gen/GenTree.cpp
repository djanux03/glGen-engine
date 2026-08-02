// tree.v1 — conifers, broadleaf trees, saplings and dead snags.
//
// A recursive branching skeleton emitted as tapered tubes (bark submesh),
// followed by foliage placed at the collected branch tips (foliage submesh,
// alpha-cutout). Two submeshes rather than one because bark and leaves need
// different materials, and because the vegetation pipeline wants foliage it
// can treat as cutout geometry.
//
// The two passes are not an accident of style: MeshBuilder has a single
// "current submesh", so all bark must be emitted before the foliage submesh
// opens. Pass 1 grows the skeleton and records tips; pass 2 dresses them.

#include "../GenRandom.h"
#include "../GeneratorRegistry.h"
#include "../GeneratorSchema.h"
#include "../MeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

namespace gen {
namespace {

struct Tip {
  glm::vec3 pos;
  glm::vec3 dir;
  int level;
  float scale; // relative foliage size at this tip
};

struct TreeParams {
  int levels = 3;
  int branchesPerLevel = 4;
  float branchAngleDeg = 45.0f;
  float lengthRatio = 0.62f;
  float radiusRatio = 0.58f;
  float startHeight = 0.35f;
  float crook = 0.25f;
  int radialSegments = 6;
  int segmentsPerBranch = 3;
};

// Rotates `dir` by `angleDeg` about an axis perpendicular to it, chosen by
// `roll` -- how a child branch leaves its parent.
glm::vec3 branchOff(const glm::vec3 &dir, float angleDeg, float rollRad) {
  const glm::vec3 ref = (std::fabs(dir.y) > 0.99f) ? glm::vec3(1, 0, 0)
                                                   : glm::vec3(0, 1, 0);
  const glm::vec3 side = glm::normalize(glm::cross(ref, dir));
  const glm::vec3 up = glm::cross(dir, side);
  const glm::vec3 axis = side * std::cos(rollRad) + up * std::sin(rollRad);
  const glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(angleDeg), axis);
  return glm::normalize(glm::vec3(rot * glm::vec4(dir, 0.0f)));
}

void growBranch(MeshBuilder &out, GenRandom &rng, const TreeParams &tp,
                glm::vec3 base, glm::vec3 dir, float length, float radius,
                int level, std::vector<Tip> &tips) {
  const int segs = std::max(1, tp.segmentsPerBranch);
  // Deeper branches get cheaper tubes -- a twig at 6 sides costs as much as
  // the trunk and is a few pixels wide.
  const int radial = std::max(3, tp.radialSegments - level);

  const float segLen = length / static_cast<float>(segs);
  glm::vec3 p = base;
  glm::vec3 d = dir;
  float r = radius;

  // Where children attach, in this branch's own parameter space.
  std::vector<std::pair<glm::vec3, glm::vec3>> attachments;

  for (int i = 0; i < segs; ++i) {
    const float t0 = static_cast<float>(i) / static_cast<float>(segs);
    const float t1 = static_cast<float>(i + 1) / static_cast<float>(segs);
    const float r0 = radius * (1.0f - t0 * (1.0f - tp.radiusRatio));
    const float r1 = radius * (1.0f - t1 * (1.0f - tp.radiusRatio));

    // Wander: a small random turn per segment. This is what separates a tree
    // from a diagram of a tree -- perfectly straight limbs read as CG.
    const glm::vec3 wobble = rng.inSphere() * tp.crook * 0.35f;
    d = glm::normalize(d + wobble);
    const glm::vec3 next = p + d * segLen;

    // Cap only the ends: interior caps are inside solid geometry, and at
    // hundreds of segments per tree they are pure waste.
    out.addTaperedCylinder(p, r0, next, r1, radial, /*capStart=*/i == 0,
                           /*capEnd=*/i == segs - 1, /*vTile=*/segLen * 2.0f);

    if (t1 > tp.startHeight)
      attachments.emplace_back(next, d);
    p = next;
    r = r1;
  }

  if (level >= tp.levels || attachments.empty()) {
    tips.push_back({p, d, level, 1.0f});
    return;
  }

  const int count = std::max(1, tp.branchesPerLevel);
  // Golden-angle phyllotaxis: successive branches are offset by ~137.5
  // degrees, which is what real plants do and what stops the branches from
  // stacking into visible vertical rows.
  const float golden = 2.39996f;
  for (int i = 0; i < count; ++i) {
    const auto &attach = attachments[rng.index(static_cast<int>(attachments.size()))];
    const float roll = golden * static_cast<float>(i) + rng.range(-0.3f, 0.3f);
    const float angle = tp.branchAngleDeg * rng.range(0.75f, 1.25f);
    const glm::vec3 childDir = branchOff(attach.second, angle, roll);
    const float childLen = length * tp.lengthRatio * rng.range(0.8f, 1.15f);
    const float childRadius = radius * tp.radiusRatio * rng.range(0.8f, 1.1f);
    if (childLen < 0.03f || childRadius < 0.004f) {
      tips.push_back({attach.first, childDir, level + 1, 0.6f});
      continue;
    }
    growBranch(out, rng, tp, attach.first, childDir, childLen, childRadius,
               level + 1, tips);
  }
  // The parent's own tip still carries foliage; without this the crown has a
  // hole exactly where the leading shoot should be.
  tips.push_back({p, d, level, 0.8f});
}

} // namespace

void registerTreeGenerator() {
  GeneratorInfo info;
  info.name = "tree.v1";
  info.description =
      "A tree: recursive branching trunk plus a foliage crown. Covers "
      "conifers, broadleaf trees, saplings and bare dead snags. Emits two "
      "submeshes (bark, foliage) so they can take different materials. Height "
      "is in metres and the pivot is at the base, ready to place on terrain.";
  info.polyBudget = 4000;
  info.schema =
      SchemaBuilder()
          .number("height", 8.0f, 0.3f, 40.0f, "Total height in metres.")
          .number("trunkRadius", 0.22f, 0.01f, 3.0f,
                  "Radius at the base in metres.")
          .number("trunkTaper", 0.45f, 0.05f, 0.95f,
                  "How much narrower the trunk gets toward the top. Low values "
                  "give a columnar mature conifer, high values a spindly "
                  "sapling.")
          .integer("trunkSegments", 4, 1, 10,
                   "Sections the trunk is built from. More allows more bend.")
          .number("lean", 0.0f, 0.0f, 1.0f,
                  "Overall tilt off vertical. Use for windswept or "
                  "cliff-edge trees.")
          .number("leanDirDeg", 0.0f, 0.0f, 360.0f,
                  "Compass direction the tree leans toward.")
          .number("crook", 0.25f, 0.0f, 1.0f,
                  "How much limbs wander. 0 is a perfectly straight diagram, "
                  "high values are gnarled and characterful.")
          .integer("branchLevels", 3, 0, 4,
                   "Recursion depth. 0 is a bare pole, 3 is a full tree. Each "
                   "level multiplies the triangle count.")
          .integer("branchesPerLevel", 4, 1, 8,
                   "Child branches grown from each limb.")
          .number("branchAngleDeg", 48.0f, 5.0f, 90.0f,
                  "How far children swing away from their parent. Small "
                  "angles give a narrow upright crown, large ones a spreading "
                  "canopy.")
          .number("branchLengthRatio", 0.62f, 0.2f, 0.95f,
                  "Child length relative to parent.")
          .number("branchStartHeight", 0.35f, 0.0f, 0.95f,
                  "Fraction of the trunk kept clear before branching starts. "
                  "High values give the bare lower trunk of a forest conifer.")
          .integer("radialSegments", 6, 3, 12,
                   "Sides per limb. 6 suits the engine's faceted look; raise "
                   "only for hero trees.")
          .enumString("canopy", "conifer",
                      {"conifer", "broadleaf", "blob", "bare"},
                      "Foliage style. 'conifer' stacks needle skirts down the "
                      "upper trunk, 'broadleaf' clusters leaf cards at branch "
                      "tips, 'blob' uses cheap stylised masses, 'bare' is a "
                      "dead snag with no foliage.")
          .number("canopyDensity", 0.8f, 0.0f, 1.0f,
                  "How much foliage is placed. Low values read as sparse, "
                  "late-autumn or unhealthy.")
          .number("leafSize", 0.45f, 0.03f, 3.0f,
                  "Size of an individual foliage element in metres.")
          .color("barkColor", glm::vec3(0.32f, 0.24f, 0.17f),
                 "Bark base colour, linear RGB.")
          .color("foliageColor", glm::vec3(0.20f, 0.42f, 0.16f),
                 "Foliage base colour, linear RGB.")
          .schema();

  info.build = [](const Params &p, uint32_t seed,
                  std::vector<std::string> &warnings,
                  std::string &) -> std::unique_ptr<MeshData> {
    MeshBuilder out;
    GenRandom rng(seed);

    const float height = p.num("height", 8.0f);
    const float trunkRadius = p.num("trunkRadius", 0.22f);
    const std::string canopy = p.str("canopy", "conifer");
    const float leafSize = p.num("leafSize", 0.45f);
    const float density = p.num("canopyDensity", 0.8f);

    TreeParams tp;
    tp.levels = p.integer("branchLevels", 3);
    tp.branchesPerLevel = p.integer("branchesPerLevel", 4);
    tp.branchAngleDeg = p.num("branchAngleDeg", 48.0f);
    tp.lengthRatio = p.num("branchLengthRatio", 0.62f);
    tp.radiusRatio = 1.0f - p.num("trunkTaper", 0.45f);
    tp.startHeight = p.num("branchStartHeight", 0.35f);
    tp.crook = p.num("crook", 0.25f);
    tp.radialSegments = p.integer("radialSegments", 6);
    tp.segmentsPerBranch = p.integer("trunkSegments", 4);

    MaterialAsset bark;
    bark.id = "bark";
    bark.baseColor = glm::vec4(p.color("barkColor", glm::vec3(0.32f, 0.24f, 0.17f)), 1.0f);
    bark.roughness = 0.9f;
    out.beginSubmesh("Bark", bark);

    const float leanRad = glm::radians(p.num("leanDirDeg", 0.0f));
    const float lean = p.num("lean", 0.0f);
    glm::vec3 dir = glm::normalize(
        glm::vec3(std::sin(leanRad) * lean * 0.8f, 1.0f,
                  std::cos(leanRad) * lean * 0.8f));

    std::vector<Tip> tips;
    growBranch(out, rng, tp, glm::vec3(0.0f), dir, height, trunkRadius, 0, tips);

    if (canopy == "bare" || density <= 0.0f)
      return out.build();

    MaterialAsset foliage;
    foliage.id = "foliage";
    foliage.baseColor =
        glm::vec4(p.color("foliageColor", glm::vec3(0.20f, 0.42f, 0.16f)), 1.0f);
    foliage.roughness = 0.75f;
    // Cutout rather than blended: foliage cards must not sort, and the
    // vegetation path expects a cutout convention (ScatterLayer::alphaCutout).
    foliage.alphaCutoff = 0.5f;
    out.beginSubmesh("Foliage", foliage);

    if (canopy == "conifer") {
      // Needle skirts stacked down the upper trunk. This reads correctly at
      // the engine's faceted low-poly style -- the shipped pines are exactly
      // this shape -- and costs a fraction of per-tip leaf cards.
      const float crownBase = height * (0.15f + 0.25f * (1.0f - density));
      const float crownTop = height * 1.0f;
      const int skirts = std::max(2, static_cast<int>(3 + density * 5.0f));
      for (int i = 0; i < skirts; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(skirts);
        const float y0 = glm::mix(crownBase, crownTop, t);
        const float y1 = glm::mix(crownBase, crownTop, t + 1.4f / skirts);
        // Skirts shrink toward the top, and each overhangs the one above it.
        const float r = leafSize * 2.2f * (1.0f - t * 0.85f) * (0.7f + density * 0.5f);
        const glm::vec3 jitter(rng.range(-0.05f, 0.05f) * r, 0.0f,
                               rng.range(-0.05f, 0.05f) * r);
        // Lean carries the crown with the trunk.
        const glm::vec3 axis = dir * y0;
        out.addTaperedCylinder(glm::vec3(axis.x, y0, axis.z) + jitter, r,
                               glm::vec3(axis.x, std::min(y1, crownTop), axis.z) + jitter,
                               r * 0.15f, std::max(4, tp.radialSegments),
                               /*capStart=*/true, /*capEnd=*/false);
      }
    } else if (canopy == "blob") {
      for (const Tip &tip : tips) {
        if (!rng.chance(density))
          continue;
        out.addIcosphere(tip.pos + tip.dir * leafSize * 0.4f,
                         leafSize * tip.scale, 1);
      }
      out.recomputeSmoothNormals();
    } else { // broadleaf
      // A card is a CLUSTER of leaves, not one leaf: at one-leaf scale a tree
      // needs thousands of them to read as a canopy, which no poly budget
      // here survives. Sized and spread accordingly.
      const float clusterSize = leafSize * 1.9f;
      const int cardsPerTip = std::max(2, static_cast<int>(3 + density * 8.0f));
      for (const Tip &tip : tips) {
        if (!rng.chance(0.45f + density * 0.55f))
          continue;
        for (int i = 0; i < cardsPerTip; ++i) {
          const glm::vec3 at = tip.pos + tip.dir * (clusterSize * 0.35f) +
                               rng.inSphere() * clusterSize * 0.85f;
          // Random yaw + a downward droop, so a cluster reads as leaves
          // hanging rather than a card sculpture. Two cards crossed at ~90
          // degrees would read better still, but doubles the triangle cost
          // for a silhouette this style does not need.
          glm::mat4 m = glm::translate(glm::mat4(1.0f), at);
          m = glm::rotate(m, rng.range(0.0f, 6.2831853f), glm::vec3(0, 1, 0));
          m = glm::rotate(m, glm::radians(rng.range(-40.0f, 20.0f)),
                          glm::vec3(1, 0, 0));
          out.pushTransform(m);
          // Silhouette in the geometry, not in alpha -- the mesh pipeline has
          // no cutout (see MeshBuilder::addLeafCard).
          out.addLeafCard(clusterSize * tip.scale * rng.range(0.85f, 1.35f),
                          clusterSize * tip.scale * rng.range(0.85f, 1.3f),
                          rng.range(5.0f, 25.0f));
          out.popTransform();
        }
      }
    }

    if (tp.levels == 0 && canopy == "broadleaf")
      warnings.push_back(
          "branchLevels 0 with a broadleaf canopy puts all foliage at the "
          "single trunk tip, which reads as a lollipop -- raise branchLevels "
          "or use canopy 'conifer'");
    if (p.num("branchStartHeight", 0.35f) > 0.85f && tp.levels > 0)
      warnings.push_back(
          "branchStartHeight above 0.85 leaves almost no trunk to branch "
          "from, so most branches will bunch at the very top");

    return out.build();
  };

  GeneratorRegistry::instance().registerGenerator(std::move(info));
}

} // namespace gen
