#pragma once

#include "VulkanAccel.h"
#include "VulkanBindless.h"
#include "VulkanMesh.h"
#include "VulkanPipelineCache.h"
#include "VulkanSwapchain.h"

#include <vk_mem_alloc.h>

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct MeshData; // engine CPU model data (Engine/Assets/MeshData.h)

namespace vkrhi {

// Phase 2 renderer. Frame = shadow pass (cascaded depth) -> scene pass (HDR,
// shadowed) -> tonemap pass (swapchain). Offscreen targets are per-frame-in-
// flight so the two in-flight frames never alias.
class VulkanRenderer {
public:
  // assetDir is the engine asset root (the runtime's GLGEN_VK_ASSET_DIR).
  // Only the volumetric cloud noise is loaded from it, and only optionally:
  // pass "" (the default, which the smoke test does) and the cloud passes are
  // skipped, leaving the sky exactly as it renders without them.
  bool init(VulkanContext &ctx, VkSurfaceKHR surface,
            const std::string &shaderDir,
            std::function<void(uint32_t &, uint32_t &)> queryFramebufferSize,
            const std::string &assetDir = std::string());

  // --- engine-drivable scene API ---------------------------------------
  // Build the scene after init(): create meshes, place instances, then call
  // finalizeScene() to build the ray-tracing acceleration structures.
  // For dynamic scenes, call clearInstances() + addInstance() every frame:
  // drawFrame() rebuilds the current frame's TLAS from the instance list.
  // finalizeScene() may be called again after adding meshes (it waits for
  // the device and rebuilds the BLAS set).
  using MeshHandle = uint32_t;
  MeshHandle createMeshFromObj(const std::string &path);
  // Uploads the engine's parsed CPU model data (any source format, authored
  // scale, embedded textures) — the Vulkan half of the parse/upload split.
  MeshHandle createMeshFromData(const ::MeshData &data,
                                const std::string &debugName);
  // Replaces an existing mesh's vertex/index buffers and drawItems in place
  // (destroys the old buffers, uploads the new ones) and rebuilds its BLAS
  // at the same slot (VulkanAccel::updateBlas()) -- `handle` stays valid and
  // stable, unlike creating a new mesh. For terrain-brush chunk rebuilds:
  // the chunk keeps its existing MeshHandle/cache entry, just with fresh
  // geometry. Returns false if `handle` is out of range or `data` has no
  // geometry.
  bool updateMeshFromData(MeshHandle handle, const ::MeshData &data);
  // Destroys a mesh's GPU buffers and releases its BLAS slot (both deferred
  // past the frames still in flight) and recycles `handle` for a future
  // createMeshFromData(). The caller must stop adding instances for it --
  // draw loops and TLAS assembly skip dead handles defensively, but a
  // recycled handle will silently point at the NEW mesh. Terrain streaming
  // calls this on chunk unload/LOD replacement so GPU memory tracks the
  // active chunk set instead of growing forever.
  void destroyMesh(MeshHandle handle);
  void addInstance(MeshHandle mesh, const glm::mat4 &transform,
                   bool isTerrain = false);
  void clearInstances() { mInstances.clear(); }

  // Per-instance data for the GPU-instanced vegetation/scatter pipeline
  // (binding 1, VK_VERTEX_INPUT_RATE_INSTANCE). R4 (MEADOW_TERRAIN_REVAMP_
  // PLAN.md §6c.5 + the R3-deferred per-instance biome lighting): alongside
  // the model matrix, every instance carries a small color-jitter tint and
  // the biome weights baked at its placement position, both consumed by
  // meshInstanced.frag (a fork of mesh.frag -- see that file for why a
  // fork was needed rather than adding varyings to the shared mesh.frag).
  struct VegInstanceGpu {
    glm::mat4 model{1.0f};
    glm::vec4 colorJitter{1.0f}; // rgb tint, w unused
    glm::vec4 biomeWeights{1.0f, 0.0f, 0.0f, 0.0f}; // x=wMeadow y=wForest z=wMountain
  };

  // GPU-instanced vegetation/scatter (Phase 3, extended R4): each batch is
  // every currently-placed instance of one mesh. Call once per frame;
  // replaces the previous frame's batches entirely. Internally memcpy's
  // instance data into a per-mesh host-visible buffer (growing it like a
  // std::vector on overflow) and records one vkCmdDrawIndexed(instanceCount=N)
  // per submesh per batch in drawFrame() -- draw-call count is independent
  // of how many instances are placed. Each instance also gets a TLAS
  // InstanceInput (same mesh's BLAS, one entry per placement) so ray-traced
  // shadows stay correct; that's cheap (no new BLAS), unlike the raster
  // draw-call count this API exists to avoid.
  // A contiguous run of one batch's instances that came from the same
  // terrain chunk, with its world-space bounds -- the unit of vegetation
  // frustum/distance culling (per-chunk, not per-plant: cheap to test, and
  // adjacent visible runs merge back into one instanced draw).
  struct VegRange {
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    uint32_t first = 0;
    uint32_t count = 0;
  };
  struct VegBatch {
    MeshHandle mesh;
    std::vector<VegInstanceGpu> instances;
    std::vector<VegRange> ranges; // covers `instances` in order, chunk-grouped

    // R5 per-batch overrides. These exist because grass and trees cannot
    // share one global setting: grass must cut out at ~90m and contribute no
    // ray-traced shadow, while trees must stay visible to the far plane and
    // must cast.
    //
    // Draw distance for THIS batch, overriding Params::vegDrawDistance.
    // <= 0 = use the global value.
    float drawDistance = 0.0f;
    // false = this batch never enters the TLAS, so it neither casts nor
    // receives ray-traced shadows. The TLAS takes one instance per
    // placement; at grass instance counts that is the difference between a
    // usable frame time and an unusable one.
    bool rayTracedShadows = true;
    // Vertex wind sway, consumed by meshInstanced.vert. 0 = static geometry
    // (the pipeline is shared; a zero strength short-circuits in the shader).
    // windStrength is meters of displacement at the plant's tip;
    // windMeshHeight (the UNSCALED mesh's Y extent) is what lets the shader
    // normalize height into a 0..1 fraction so that unit holds for both a
    // grass clump and a conifer.
    float windStrength = 0.0f;
    float windSpeed = 1.0f;
    float windMeshHeight = 1.0f;
    // Root darkening, 0..1 (ScatterLayer::groundOcclusion). The cheap
    // stand-in for the contact shadow a batch excluded from the TLAS cannot
    // cast on itself; see meshInstanced.frag.
    float groundOcclusion = 0.0f;
    float foliageSssStrength = 1.0f;
  };
  // Call when the scatter set CHANGES (chunk with vegetation streamed in/
  // out), not per frame -- uploads and TLAS-input caching key off it.
  void setVegetationBatches(const std::vector<VegBatch> &batches);

  // --- editor debug lines ------------------------------------------------
  // Depth-tested world-space lines: the editor grid, origin axes, collider
  // outlines, anything that has to sit IN the scene rather than on top of it.
  //
  // This exists because the editor's overlays were all CPU-projected into
  // ImGui's foreground draw list, which cannot depth test -- every line drew
  // through terrain and geometry. A grid drawn that way reads as broken
  // immediately, since the one thing a grid must do is lie on the ground.
  struct DebugLineVertex {
    glm::vec3 pos;
    glm::vec4 color; // alpha is modulated by distance fade in the shader
  };
  // Replaces the whole line set; call once per frame with everything to draw.
  // Depth-tested against the scene but does NOT write depth, so lines never
  // occlude geometry drawn after them.
  void setDebugLines(const std::vector<DebugLineVertex> &lines);
  // Distance at which lines have faded to nothing. 0 disables the fade.
  void setDebugLineFadeDistance(float metres) { mDebugLineFade = metres; }
  bool finalizeScene();
  // Incremental counterpart to finalizeScene(): builds BLAS only for meshes
  // added since the last finalizeScene()/growScene() call (via
  // VulkanAccel::appendBlas(), no vkDeviceWaitIdle, no acceleration-structure
  // teardown), then re-records this frame's TLAS. Falls back to
  // finalizeScene() if the scene hasn't been finalized yet. Safe to call
  // every frame that new meshes were added -- unlike finalizeScene(), this
  // does not stall or rebuild BLAS for meshes that already have one.
  bool growScene();
  bool sceneReady() const { return mSceneReady; }

  void drawFrame();
  void waitIdle();
  void shutdown();

  // One-shot debug capture: the next drawFrame copies the presented image to a
  // PNG at `path`. Used to visually verify rendering headlessly.
  //
  // `maxDimension` (0 = native resolution) box-downscales the image so its
  // longest side is at most that many pixels. Exists for the agent bridge: a
  // native-resolution capture is ~1.4 MB, which is far too much to inline into
  // a model's context several times per turntable.
  void requestCapture(const std::string &path, uint32_t maxDimension = 0);

  // Live, UI/input-driven render parameters. Defaults reproduce the original
  // hardcoded look exactly.
  struct Params {
    struct TerrainPaintLayer {
      glm::vec3 lit{0.30f, 0.52f, 0.18f};
      glm::vec3 shade{0.12f, 0.25f, 0.16f};
      float mottleScale = 8.0f;
      float overlayStrength = 0.0f;
    };
    struct StyleParams {
      std::array<TerrainPaintLayer, 5> terrain{{
          {{0.35f, 0.58f, 0.20f}, {0.13f, 0.29f, 0.17f}, 8.0f, 0.0f},
          {{0.25f, 0.38f, 0.16f}, {0.10f, 0.20f, 0.16f}, 7.0f, 0.0f},
          {{0.48f, 0.34f, 0.20f}, {0.25f, 0.19f, 0.18f}, 6.0f, 0.0f},
          {{0.48f, 0.50f, 0.55f}, {0.25f, 0.30f, 0.39f}, 9.0f, 0.0f},
          {{0.55f, 0.52f, 0.47f}, {0.29f, 0.31f, 0.36f}, 8.0f, 0.0f},
      }};
      float autumnAmount = 0.42f, facetStrength = 0.78f;
      float washEdgeDarkening = 0.18f, worldPaperStrength = 0.045f;
      float vibrance = 0.15f, splitBalance = 0.48f;
      glm::vec3 splitShadow{0.80f, 0.86f, 1.0f};
      glm::vec3 splitHighlight{1.0f, 0.90f, 0.72f};
      glm::vec3 outlineColor{0.075f, 0.085f, 0.12f};
      float outlineWidth = 1.0f, outlineStrength = 0.72f;
      float outlineDepthThreshold = 0.012f, outlineNormalThreshold = 0.22f;
      float outlineDistance = 220.0f, screenPaperStrength = 0.035f;
      bool fxaaEnabled = true;
      glm::vec3 skyZenith{0.20f, 0.46f, 0.62f};
      glm::vec3 skyHorizon{0.93f, 0.72f, 0.49f};
      float skyGradeStrength = 0.62f, skyBands = 5.0f;
      float skyBandSoftness = 0.30f, sunSoftness = 0.45f;
      float cloudCoverage = 0.47f, cloudSoftness = 0.18f;
      // Deck geometry + density. cloudDeckHeight is the BOTTOM of the cloud
      // layer in meters; the volumetric marcher (clouds.frag) extends it up
      // by cloudLayerThickness, and sky.frag's analytic deck -- which now
      // only draws into the environment cubemap -- treats it as its single
      // deck altitude. cloudFeatureScale and cloudOpticalDensity are used by
      // that analytic deck alone; the volumetric pass has its own scales
      // below. sunOcclusion is shared: both scale their toward-the-sun
      // extinction by it.
      float cloudDeckHeight = 1500.0f;
      float cloudFeatureScale = 1500.0f;
      float cloudOpticalDensity = 1.0f;
      float cloudSunOcclusion = 1.0f;
      glm::vec2 cloudWind{0.012f, 0.006f};
      glm::vec3 cloudLit{1.0f, 0.88f, 0.68f};
      glm::vec3 cloudMid{0.71f, 0.72f, 0.73f};
      glm::vec3 cloudBase{0.39f, 0.43f, 0.52f};

      // --- volumetric cloudscape (clouds.frag) ---------------------------
      // A Nubis-style raymarch: Perlin-Worley profile x height gradient x
      // weather map, eroded by the four-channel Nubis detail noise, lit with
      // the Nubis2/3 light-energy model. Half-resolution, composited over the
      // sky before geometry.
      bool cloudVolumetricEnabled = true;
      // 0..1 fade for the whole layer. 0 leaves the sky bare (and skips the
      // pass entirely), 1 is the full deck.
      float cloudStrength = 1.0f;
      // Vertical extent above cloudDeckHeight. 3 km over a 1.5 km base is a
      // fair-weather cumulus field with room for the tall type to tower.
      float cloudLayerThickness = 3000.0f;
      // Meters per tile of each noise volume. shapeScale sets how big a whole
      // cloud is; detailScale how fine the billows carved into it are. The
      // two are deliberately three orders of magnitude apart -- that ratio is
      // what makes the erosion read as surface detail rather than as a second
      // layer of clouds.
      float cloudShapeScale = 4200.0f;
      float cloudDetailScale = 90.0f;
      float cloudWeatherScale = 26000.0f;
      // Extinction per METER per unit of eroded density. Real cumulus sits
      // around 0.05; at that value a single 80 m step through solid cloud is
      // already opaque, which is correct but leaves the dial no travel, so
      // the default is lower and the slider reaches up to it.
      float cloudDensityMultiplier = 0.028f;
      // Optical depth toward the light through a full-density cone. Unlike
      // the above this is dimensionless (see clDensityToLight): it is what
      // sets how deep self-shadowing goes, i.e. the contrast between a lit
      // top and a shadowed base, independent of the layer's authored
      // thickness.
      float cloudLightAbsorption = 4.0f;
      float cloudAmbientStrength = 1.0f;
      // How far the curl field shears the layer's thin bases into wisps.
      float cloudCurlStrength = 0.35f;
      // Phase: the broad forward lobe, plus a tight second lobe that only
      // fires within a few degrees of the light. That second one IS the
      // silver lining on a backlit cloud edge.
      float cloudPhaseG = 0.62f;
      float cloudSilverIntensity = 1.27f;
      float cloudSilverSpread = 1.32f;
      float cloudPowderStrength = 0.55f;
      // Marching budget. maxMarchDist caps a near-horizon ray, which would
      // otherwise cross an effectively unbounded slab and force a step size
      // that dissolves the deck into stripes exactly where it is densest.
      float cloudMaxMarchDist = 55000.0f;
      float cloudMaxSteps = 96.0f;
      float cloudLightTaps = 5.0f;
      // Biases the weather map's type channel: negative flattens the sky
      // toward stratus, positive builds it toward cumulus.
      float cloudTypeBias = 0.0f;
      float cloudDetailStrength = 0.72f;
    } style;

    // Camera pose. Editor mode: driven by EditorCamera (RMB look / MMB pan /
    // scroll dolly, mouse-only). Play mode: synced from the player entity.
    glm::vec3 camPos = glm::vec3(0.0f, 0.7f, 3.0f);
    float camYawDeg = 180.0f;   // facing -Z toward the origin
    float camPitchDeg = -8.0f;
    // Incremented by scripting/RPC camera writes. main.cpp watches this and
    // reseeds EditorCamera before its next update; otherwise the editor's
    // smoothed target overwrites render.setParams before a deferred capture.
    uint64_t externalCameraRevision = 0;
    float fovDeg = 55.0f;
    // Was a hardcoded 420 (sized only for the default viewDistanceChunks=6,
    // 384m radius): streamed terrain beyond whatever this is set to is
    // culled and uploaded but literally unrenderable, no matter how large
    // TerrainSettings::viewDistanceChunks is. VkTerrainSubsystem sets this
    // from the actual streamed radius (+ margin) once terrain initializes.
    float farPlane = 420.0f;

    // Lighting & ray-traced shadows. sunIntensity scales the sun's
    // top-of-atmosphere radiance (base kSunOuterRadiance = 20 HDR); the
    // per-frame direct-light color comes from real atmospheric transmittance
    // (atmSunTransmittanceCpu), so dawn/dusk light is warm without any
    // hand-tuned tint.
    float lightYawDeg = 215.0f;
    float lightPitchDeg = 35.0f;
    // Auto-advancing time of day: when enabled, main.cpp's frame loop drives
    // lightPitchDeg forward by timeOfDaySpeed degrees/second (wrapped to
    // +-180) instead of it being a fixed manual value. The existing sun/moon
    // handoff (VulkanRenderer.cpp's drawFrame) already reacts continuously
    // to lightPitchDeg, so a full 360 degree sweep is a complete day/night
    // cycle with no separate cycle logic needed.
    bool timeOfDayEnabled = false;
    float timeOfDaySpeed = 3.0f; // deg/sec; 360/speed = seconds per full cycle
    float sunIntensity = 1.0f;
    // Scales the sky-cubemap diffuse irradiance (real sky radiance now --
    // 1.0 is the physically-consistent default; the old 0.4 belonged to the
    // hand-tuned gradient sky).
    float ambientIntensity = 1.0f;
    float shadowStrength = 1.0f;  // 0 = shadows off, 1 = full occlusion
    float shadowSoftness = 0.02f; // sun angular radius; 0 = sharp single ray
    int shadowSamples = 4;        // rays per pixel when soft

    // Small authored lights for lamps and practicals.  A fixed four-light
    // budget keeps FrameData deterministic and is enough for MCP-authored
    // showcase scenes without introducing a second scene-light registry.
    struct PointLight {
      glm::vec3 position = glm::vec3(0.0f);
      float radius = 8.0f;
      glm::vec3 color = glm::vec3(1.0f, 0.75f, 0.45f);
      float intensity = 80.0f;
    };
    std::array<PointLight, 4> pointLights{};
    uint32_t pointLightCount = 0;

    // Screen-space ambient occlusion (darkens ambient-only, never direct light).
    float aoRadius = 0.5f;   // view-space hemisphere radius
    float aoBias = 0.025f;   // self-occlusion bias
    float aoStrength = 1.0f; // 0 = off, 1 = full effect

    // Atmosphere / sky (skyModel.glsl + sky.frag).
    float atmosphereHaze = 0.30f;   // 0 = alpine-clear, 1 = heavy humid haze (Mie)
    float skyBrightness = 1.0f;     // multiplier on sky in-scatter (sky + IBL)
    float sunDiscIntensity = 20.0f; // sun disc radiance x view transmittance
    float starIntensity = 1.0f;     // night starfield + milky way
    float moonIntensity = 1.0f;     // moon disc brightness + moonlight radiance
    float moonGlowIntensity = 0.08f; // halo around the moon disc
    // Night floor (starlight/airglow) so moonless night zeniths read deep
    // blue instead of void black. Replaces the old dead nightSkyBrightness/
    // duskStrength pair (dusk color now comes from physics).
    float nightSkyBrightness = 1.0f;
    float duskStrength = 0.6f; // DEAD (kept: FrameData packing slot)

    // Sky-cubemap IBL (per-frame 128x128x6 env map with a full mip chain).
    float iblSpecularIntensity = 1.0f; // reflections dial (diffuse uses ambientIntensity)
    // Terrain-only sky-reflection dial: scales the terrain shader's whole
    // IBL contribution (diffuse irradiance*albedo AND specular envSpec)
    // independent of iblSpecularIntensity/ambientIntensity above, which also
    // drive props (mesh.frag). Lower than 1.0 by default -- a bright sky
    // reflected at full IBL strength across a wide-open, mostly-flat ground
    // mesh reads as a washed-out white ground, which props (much smaller
    // screen coverage, more varied normals) don't suffer from the same way.
    float terrainSkyReflectIntensity = 0.2f;

    // Ray-traced volumetric light scattering (god rays): half-res raymarch
    // with a ray-query shadow test per step, composited additively before
    // bloom. The medium is the same height fog the surface shaders use.
    bool volumetricEnabled = true;
    // Raised further still (was 0.7, then 3.0) -- pushed again since the
    // last pass still read as too subtle. No shader-side clamp on this
    // multiplier (see volumetric.frag), so it scales linearly all the way
    // to the tonemapper; the editor slider ceiling was raised to match.
    float volumetricIntensity = 4.5f; // 0 = off
    // Tighter forward lobe (was 0.6, then 0.8) = a sharper, more directional
    // beam toward the sun instead of a soft ambient haze -- reads as
    // visible rays rather than just brighter fog. 0.95 is effectively the
    // shader's ceiling (the HG denominator only degenerates at g=1 looking
    // exactly down-sun).
    float volumetricAnisotropy = 0.87f; // HG g: forward-scatter around the sun
    float volumetricMaxDist = 170.0f;   // meters marched from the camera
    int volumetricSteps = 18;           // shadow rays per half-res pixel
    // Scales the scattering medium's density INDEPENDENTLY of the surface
    // height-fog dial (volumetric.frag used to inherit fogParams.x*0.25
    // outright -- cranking ray visibility meant fogging the whole scene
    // too). >1 = thicker/more visible shafts (and a shorter effective
    // range, since more of the medium extinguishes); default already
    // meaningfully thicker than the old implicit 1.0.
    float volumetricDensityScale = 1.75f;
    // Independent multiplier on how much the medium thins with altitude
    // (0 = uniform density at all heights regardless of surface fog's own
    // falloff, matching the surface dial = 1).
    float volumetricHeightFalloffScale = 1.0f;
    // Organic shaft movement: a slowly drifting 3D noise field perturbs the
    // scattering medium's density along each march step (see volumetric.frag),
    // so the shafts shimmer/waver like real dust- or mist-borne light rather
    // than reading as a static, geometrically-fixed cone. turbulence is the
    // 0..1 strength of the perturbation; windSpeed is how fast the noise
    // field drifts (world units/sec along a fixed diagonal wind direction).
    float volumetricTurbulence = 0.35f;
    float volumetricWindSpeed = 0.15f;
    // Artistic tint blended into the (normally purely physical, sun/moon-
    // radiance-colored) scattered light. Strength 0 = untouched physical
    // color; defaults to a subtle warm push rather than fully overriding it.
    glm::vec3 volumetricTintColor = glm::vec3(1.0f, 0.85f, 0.55f);
    float volumetricTintStrength = 0.3f; // 0..1

    // Fog. Two media over one view ray -- see shaders/vulkan/fog.glsl for the
    // model and for what was wrong with the version this replaced.
    //
    // GROUND LAYER: mist that pools in low ground. fogDensity is its
    // extinction coefficient (per meter) at fogHeightRef, thinning upward
    // with scale height 1/fogHeightFalloff.
    float fogDensity = 0.006f;
    float fogStart = 30.0f;
    // Now a FLOOR on transmittance, not a ceiling on a blend: 1 lets fog run
    // to completion so distant terrain dissolves into a matching horizon, 0
    // disables fog entirely (which is what GLGEN_SMOKE_NOFOG and the
    // turntable rely on), and values between cap how much of a surface fog is
    // allowed to take. It defaulted to 0.9 under the old meaning, which is
    // precisely what produced the horizon seam -- terrain frozen at 90%
    // fogged against a sky at 0%.
    float fogMaxOpacity = 1.0f;
    // Per-meter thinning of the ground layer with altitude (H = 1/this).
    // Nonzero by default now: with the correct path integral, ground fog
    // that hugs the valleys is the whole point of having a second layer, and
    // at 0 it degenerates into a uniform slab indistinguishable from the
    // aerial term. 0.045 puts the scale height around 22 m.
    float fogHeightFalloff = 0.045f;
    float fogHeightRef = -2.0f; // altitude the ground layer is anchored at
                                // (TerrainSettings::seaLevel)
    // Ground-layer tint, blended by sun elevation. These two have existed and
    // been uploaded every frame since the first fog implementation without a
    // single shader reading them; fog.glsl finally does.
    glm::vec3 fogDayColor = glm::vec3(0.72f, 0.79f, 0.90f);
    glm::vec3 fogNightColor = glm::vec3(0.09f, 0.12f, 0.20f);

    // AERIAL LAYER: air, in multiples of sea-level density. Real air over the
    // ~400 m this engine renders has a Rayleigh optical depth around 0.013 --
    // physically correct and visually nothing, so the multiplier is doing
    // real work rather than papering over a wrong model. What being physical
    // still buys: extinction stays SPECTRAL (blue leaves a distant surface
    // ~3x faster than red, which is what aerial perspective is), and the
    // in-scatter converges on the sky's own radiance, so the horizon matches
    // by construction at any value of this dial.
    //
    // 180 is not arbitrary: it is the value at which the new model reproduces
    // the OLD fog's measured opacity profile down the frame (0.88 at the far
    // ridge / 0.39 near the camera, against the old 0.91 / 0.40) in the
    // mountain smoke pose. Matching it deliberately -- the point of this
    // rewrite is that the fog is correct, not that scenes suddenly have more
    // or less of it, and a default that quietly halved everyone's haze would
    // read as a regression however good the model underneath was. The old
    // 0.91 was its maxOpacity clamp, i.e. clipped; this one is still climbing.
    float fogAerialStrength = 180.0f;
    // Extra forward-Mie in-scatter toward the sun, on top of what the env
    // cubemap already carries -- the tight haze flare when looking into a low
    // sun, and the glow low mist gets from the same direction.
    float fogSunInscatter = 1.0f;
    // Henyey-Greenstein g for the ground layer's sun lobe. Lower than the
    // god rays' 0.87: mist scatters broadly, shafts are the tight case.
    float fogAnisotropy = 0.55f;
    // Patchiness of the ground layer. 0 is the analytic wash the old fog
    // always was; the default gives mist visible body without it reading as
    // noise. Scale is in cycles per meter (0.008 -> ~125 m features).
    float fogNoiseStrength = 0.4f;
    float fogNoiseScale = 0.008f;
    float fogNoiseWindSpeed = 0.35f;
    // How much of the ground layer the VISIBLE sky receives (never the env
    // cubemap -- surfaces sample that back as their own fog color). This is
    // the other half of removing the seam: terrain that fully dissolves still
    // needs something to dissolve INTO.
    float fogSkyStrength = 1.0f;

    // --- Water (shaders/vulkan/water.frag) --------------------------------
    // A single world-height water table, evaluated analytically in a
    // fullscreen pass rather than drawn as a mesh. That means one water
    // ALTITUDE for the whole world: it models a sea or a valley flood level,
    // not lakes at differing heights. Terrain streams over an unbounded XZ
    // plane, so a mesh would have needed its own chunking/LOD to match; an
    // analytic plane is exact everywhere for free and cannot crack at chunk
    // seams. Off by default -- the terrain generator has had no sea level
    // since R1, so a world that never asked for water should not grow one.
    bool waterEnabled = false;
    float waterLevel = 0.0f; // world Y of the surface
    // Shallow/deep are the colours the medium TRANSMITS at ~0 m and at
    // waterClarity metres of path; the absorption between them is
    // Beer-Lambert, so mid depths interpolate physically rather than by lerp.
    glm::vec3 waterShallowColor = glm::vec3(0.22f, 0.44f, 0.42f);
    glm::vec3 waterDeepColor = glm::vec3(0.02f, 0.09f, 0.14f);
    float waterClarity = 6.0f;   // metres to reach the deep colour
    float waterRoughness = 0.06f; // GGX roughness for sun glint + SSR blur
    // Waves are normal-only (the surface stays geometrically flat), summed
    // from a few directional octaves. Steepness scales the normal's tilt, not
    // a displacement, so large values read as chop rather than as height.
    float waveAmplitude = 0.22f;
    // Cycles per metre of the base octave. 0.09 gives ~11 m swells, which at
    // four octaves never produces anything you can see rippling; water wants
    // metre-scale detail in the near field.
    float waveScale = 0.35f;
    float waveSpeed = 0.55f;
    glm::vec2 waveDirection = glm::vec2(1.0f, 0.35f);
    // Screen-space reflections. On a miss (ray leaves the screen, or finds no
    // depth in front of it) the sky cubemap fills in, so there is never a
    // hard edge where SSR stops -- only a change in how much detail is in the
    // reflection.
    float waterReflectionStrength = 1.0f;
    int waterSsrSteps = 40;
    float waterSsrThickness = 0.7f; // metres of depth a hit may span
    // Shoreline foam, driven by how little water is between the surface and
    // the terrain behind it.
    float waterFoamDepth = 0.45f;
    float waterFoamStrength = 0.7f;

    // Post-processing (tonemap pass). Exposure re-anchored for the
    // physical-scale lighting (sun outer radiance 20 HDR at intensity 1).
    float exposure = 0.9f;
    float gamma = 2.2f;
    float saturation = 1.0f;
    float contrast = 1.0f;
    float vignette = 0.15f;
    int tonemapMode = 0; // 0=Painterly 1=ACES 2=Reinhard 3=Linear
    // Simple sun-elevation-driven auto-exposure (not histogram-based): each
    // frame, `exposure` eases toward a target derived from how high the sun
    // is. ON by default since the physically-scaled day/night range is far
    // wider than one fixed exposure can present (moonlight is ~1/250 of
    // sunlight here; real-world is far more extreme still).
    bool autoExposure = true;
    float autoExposureMin = 0.9f;  // target exposure at/above noon
    float autoExposureMax = 3.6f;  // target exposure at night
    float autoExposureSpeed = 0.05f; // per-frame ease factor toward target

    // Bloom (half-res bright-pass extract + separable blur, additive in the
    // tonemap pass before the tonemap curve).
    float bloomThreshold = 1.0f; // brightness where bloom starts contributing
    float bloomKnee = 0.5f;      // soft-knee width around the threshold
    float bloomIntensity = 0.2f; // 0 = off
    // Weight of the second, quarter-res blur pass RELATIVE to bloomIntensity
    // (a wider/softer glow, summed on top of the tight half-res one -- see
    // tonemap.frag). Was a hardcoded 0.6 multiplier; exposed so the wide
    // glow's spatial spread can be dialed down (toward 0 = tight core only)
    // without touching bloomIntensity itself.
    float bloomWideIntensity = 0.6f;

    // Terrain materials (band edges in world-height METERS / slope), scaled
    // to the chunked terrain generator (TerrainSettings: heightScale 24,
    // seaLevel -2): sand near the waterline, grass on the flats, snow on
    // the upper peaks. (The old -0.52..0.60 defaults were sized for the ±1
    // smoke-test terrain and rendered everything above y=0.6 as snow.)
    // grassStart/grassEnd/snowStart/snowEnd are DEAD (R1 -- no sand/snow
    // height bands any more); terrainMat1 still packs them (unused by the
    // shader) for the same "don't renumber FrameDataGpu offsets" reason as
    // terrainColorSand/Snow above.
    float grassStart = -1.0f;   // unused
    float grassEnd = 2.0f;      // unused
    float snowStart = 10.0f;    // unused
    float snowEnd = 16.0f;      // unused
    float rockSlopeStart = 0.35f;
    float rockSlopeEnd = 0.65f;
    // R2: repurposed from the old single-generic-detail-texture scale/
    // strength (deleted along with the flat-color material) -- same "keep
    // the field name, change the meaning" reasoning as terrainColorSand/Snow.
    // terrainDetailScale -> "Dirt Strength" (curvature-driven dirt overlay).
    // terrainDetailStrength -> "Anti-Tile Strength" (2-scale texture-bombing
    // blend factor for every image layer).
    float terrainDetailScale = 0.5f;    // -> dirt overlay strength (0..1)
    float terrainDetailStrength = 0.35f; // -> anti-tile blend strength (0..1)

    // Terrain material colors. R1 (MEADOW_TERRAIN_REVAMP_PLAN.md) deletes
    // the sand/snow bands (3-biome system has neither) -- rather than
    // remove terrainColorSand/Snow and renumber every offsetof(FrameDataGpu,
    // ...) static_assert below (see that struct's own comment on why a
    // shifted offset is a real, previously-hit bug, not just tidiness),
    // these two slots are REPURPOSED as placeholder forest-floor/scree
    // tints until R2's real per-biome ground textures land. The field names
    // stay "Sand"/"Snow" (GPU-side FrameDataGpu packing + the editor UI
    // label are what changed, not this struct) -- a cosmetic mismatch
    // that's fine for one phase, called out here so it isn't mysterious.
    glm::vec3 terrainColorSand = glm::vec3(0.10f, 0.24f, 0.09f); // -> forest floor tint
    glm::vec3 terrainColorGrass = glm::vec3(0.16f, 0.36f, 0.10f);
    glm::vec3 terrainColorRock = glm::vec3(0.33f, 0.29f, 0.25f);
    glm::vec3 terrainColorSnow = glm::vec3(0.45f, 0.42f, 0.38f); // -> scree/mountain-rock tint

    // Terrain material polish: large-scale albedo variation and rock
    // surface detail (kept from the old biome-tint-era polish pass).
    // terrainBiomeTintEnabled/Intensity are DEAD (the old 6-color biome
    // palette they drove no longer exists) -- same "don't renumber offsets"
    // reasoning as above.
    bool terrainBiomeTintEnabled = false;   // unused
    float terrainBiomeTintIntensity = 0.35f; // unused
    float terrainMacroVariationStrength = 0.15f;
    float terrainRockDetailStrength = 0.3f;

    // Pins the animation clock (cloud drift, dew twinkle, volumetric
    // turbulence, star twinkle) to a fixed value. Negative = follow the wall
    // clock, which is the normal running state. Set this before a capture that
    // has to be byte-comparable with a previous one -- without it, golden-image
    // regression cannot work at all, because the sky is never twice the same.
    float fixedTimeSeconds = -1.0f;

    // Debug visualization (frameData.glsl miscParams.x): 0=off 1=albedo
    // 2=normals 3=fog 4=SSAO 5=shadow visibility 6=NaN detector 7=biome
    // weights. While non-zero, the tonemap pass becomes a linear passthrough
    // (no exposure/grade/bloom) so the raw data is what reaches the screen.
    int debugViewMode = 0;

    // R2 (MEADOW_TERRAIN_REVAMP_PLAN.md §5): 5-layer image splat material.
    // File paths, not resolved bindless indices -- VulkanRenderer resolves
    // them lazily (resolveTerrainMaterialsIfDirty(), called once per frame,
    // a cheap no-op unless terrainMaterialsDirty is set) into the cache the
    // FrameDataGpu packing step below reads from. Empty albedoPath means
    // "use mDefaultTexIndex, no material here" (only meaningful for dirt,
    // the one slot with no shipped default -- see the plan's material
    // table). Empty normal/roughness paths mean "no map, use a flat
    // default" (normal = straight-up, roughness = a per-slot scalar isn't
    // modeled here; the shader falls back to a fixed 0.85 rough default).
    struct TerrainMaterialSlot {
      std::string albedoPath;
      std::string normalPath;
      std::string roughnessPath;
      float tiling = 8.0f; // world units per texture repeat
    };
    // Order: 0=meadow 1=forest 2=dirt 3=rock 4=scree -- matches
    // terrainTexA/B/C/D's field order in FrameDataGpu exactly.
    std::array<TerrainMaterialSlot, 5> terrainMaterialSlots;
    // Set true initially (first frame must resolve) and whenever a path is
    // edited; cleared by resolveTerrainMaterialsIfDirty() once it has
    // (re)loaded every slot.
    bool terrainMaterialsDirty = true;

    // R3 (MEADOW_TERRAIN_REVAMP_PLAN.md §5.5): biome lighting & atmosphere.
    // Per-pixel terrain response -- see FrameDataGpu's biomeAmbient*/
    // biomeDirectParams/biomeFog*/biomeMountainExtra fields for what each
    // of these packs into.
    glm::vec3 biomeAmbientTintMeadow = glm::vec3(1.04f, 1.0f, 0.94f);
    float biomeAmbientIntensityMeadow = 1.0f;
    glm::vec3 biomeAmbientTintForest = glm::vec3(0.82f, 0.95f, 1.08f);
    float biomeAmbientIntensityForest = 0.75f;
    glm::vec3 biomeAmbientTintMountain = glm::vec3(0.92f, 0.97f, 1.10f);
    float biomeAmbientIntensityMountain = 1.05f;
    float forestCanopyOcclusion = 0.5f;     // 0=fully occluded 1=no extra occlusion
    float forestLightShaftStrength = 0.5f;  // 0..1, dappled light-shaft mask strength
    float mountainDirectBoost = 1.05f;      // direct-sun multiplier at full wMountain
    float biomeLightingStrength = 1.0f;     // global 0..1 dial over all of the above
    glm::vec3 forestFogTint = glm::vec3(0.55f, 0.68f, 0.58f);
    float forestFogDensityMult = 2.0f;      // denser wood air
    glm::vec3 mountainFogTint = glm::vec3(0.78f, 0.87f, 1.0f);
    float mountainFogDensityMult = 0.5f;    // clear near air
    // Extra blue haze on distant/lower mountain terrain. Capped by
    // fogMaxOpacity (see terrainChunk.frag), so this dial trades within that
    // budget rather than adding on top of it.
    float mountainAerialStrength = 1.0f;

    // Strength of the terrain's additive ground optics (backlit blade
    // translucency, cuticle sheen, dew glint, puddle gloss -- see
    // grassMaterial.glsl). 1.0 is the calibrated default; these are layered
    // ON TOP of a complete PBR response, so raising this blows out sunlit
    // ground long before it looks brighter, and 0 leaves the ground purely
    // Cook-Torrance.
    // Camera grade (tonemap pass): a small, smoothed exposure/white-balance
    // shift toward whichever biome the CAMERA (not the surface) is
    // currently in -- sells "stepping under the canopy" as a whole-frame
    // feel on top of the per-pixel terrain response above.
    bool cameraGradeEnabled = true;
    float cameraGradeStrength = 1.0f;      // 0..1 dial
    float cameraGradeSmoothTime = 2.0f;    // seconds, exponential smoothing

    // Perf pass: vegetation reach. Only scatter instances within
    // vegShadowDistance of the camera enter the ray-traced-shadow TLAS (a
    // distant tree's shadow is subpixel long before this); vegDrawDistance
    // (0 = unlimited) additionally skips whole per-chunk ranges past that
    // distance in the raster passes.
    float vegShadowDistance = 180.0f;
    float vegDrawDistance = 0.0f;
  };
  Params &params() { return mParams; }

  // Per-frame render statistics (culling, TLAS activity, mesh slots) for
  // the editor's Statistics panel. Reset at the top of every drawFrame().
  struct FrameStats {
    uint32_t instancesDrawn = 0;  // mesh instances passing the frustum test
    uint32_t instancesCulled = 0; // mesh instances skipped by it
    uint32_t vegRangesDrawn = 0;
    uint32_t vegRangesCulled = 0;
    uint32_t vegInstancesDrawn = 0;
    uint32_t tlasInstances = 0;   // instances in the current TLAS input list
    bool tlasRebuiltThisFrame = false;
    uint32_t meshSlotsLive = 0;
    uint32_t meshSlotsFree = 0;
  };
  const FrameStats &frameStats() const { return mFrameStats; }

  // R3: called once per frame (VkTerrainSubsystem::addFrameInstances(), which
  // already owns a TerrainQuery) with the RAW biome weights at the camera's
  // current world position. VulkanRenderer owns the exponential smoothing
  // (Params::cameraGradeSmoothTime) and derives the tonemap pass's grade
  // from the smoothed result -- see drawFrame()'s tonemap push constant.
  void setCameraBiomeWeights(glm::vec3 raw) { mCameraBiomeWeightsRaw = raw; }

  // Records an overlay (e.g. ImGui) inside the tonemap pass, on the swapchain.
  void setOverlayCallback(std::function<void(VkCommandBuffer)> cb) {
    mOverlay = std::move(cb);
  }
  VkFormat swapchainColorFormat() const { return mSwapchain.imageFormat(); }
  uint32_t swapchainImageCount() const { return mSwapchain.imageCount(); }

private:
  static constexpr uint32_t kFramesInFlight = 2;

  // Mirrors the FrameData uniform block in every scene shader.
  struct FrameDataGpu {
    glm::mat4 viewProj;
    glm::mat4 view;
    glm::vec4 lightDir;      // xyz world-space, w=sun intensity
    glm::vec4 fogParams;     // x=density y=start z=maxOpacity w=heightFalloff
    glm::vec4 fogDayColor;   // rgb
    glm::vec4 fogNightColor; // rgb
    glm::vec4 lightParams;   // x=ambient y=shadowStrength z=softness w=samples
    glm::vec4 terrainMat1;   // x=grassStart y=grassEnd z=snowStart w=snowEnd
    glm::vec4 terrainMat2;   // x=rockSlopeStart y=rockSlopeEnd z=detailScale w=detailStrength
    glm::vec4 terrainColorSand;
    glm::vec4 terrainColorGrass;
    glm::vec4 terrainColorRock;
    glm::vec4 terrainColorSnow;
    // Appended (not inserted): keeps every existing field's byte offset
    // unchanged, so terrain/sky/tonemap shaders (which declare only the
    // fields they use) don't need to know this exists.
    glm::vec4 camPosWS; // xyz = camera world position (w unused)
    glm::vec4 skyAmbientParams; // x=nightSkyBrightness y=duskStrength (z/w unused)
    glm::vec4 terrainMat3; // x=biomeTintEnabled(0/1) y=biomeTintIntensity z=macroVariationStrength w=rockDetailStrength
    glm::vec4 miscParams; // x=debugViewMode y=fogHeightRef z=timeSeconds (w unused)
    // R2 (MEADOW_TERRAIN_REVAMP_PLAN.md §5): 5-layer image splat material --
    // bindless texture indices for meadow/forest/dirt/rock/scree, 3 maps
    // each (albedo/normal/roughness), resolved once by
    // VulkanRenderer::resolveTerrainMaterialsIfDirty() from
    // Params::terrainMaterialSlots (paths), not per frame. Appended (not
    // inserted) per this struct's own established rule.
    glm::uvec4 terrainTexA; // x=meadowAlbedo y=meadowNormal z=meadowRoughness w=forestAlbedo
    glm::uvec4 terrainTexB; // x=forestNormal y=forestRoughness z=dirtAlbedo w=dirtNormal
    glm::uvec4 terrainTexC; // x=dirtRoughness y=rockAlbedo z=rockNormal w=rockRoughness
    glm::uvec4 terrainTexD; // x=screeAlbedo y=screeNormal z=screeRoughness w=unused
    glm::vec4 terrainTiling0; // x=meadow y=forest z=dirt w=rock (world units per tile)
    glm::vec4 terrainTiling1; // x=scree (y/z/w unused)
    // R3 (MEADOW_TERRAIN_REVAMP_PLAN.md §5.5): biome lighting & atmosphere.
    // Per-pixel ambient/direct/fog response blended by the same
    // (wMeadow,wForest,wMountain) weights the material splat already reads
    // -- never a hard biome switch, same rule as everywhere else in this
    // system. Consumed by terrainChunk.frag via biomeLighting.glsl.
    glm::vec4 biomeAmbientMeadow;   // rgb=sky-irradiance tint w=intensity scale
    glm::vec4 biomeAmbientForest;   // rgb tint w=intensity scale (canopy swallows skylight)
    glm::vec4 biomeAmbientMountain; // rgb tint w=intensity scale
    glm::vec4 biomeDirectParams;    // x=forestCanopyOcclusion y=forestLightShaftStrength z=mountainDirectBoost w=globalStrengthDial(0..1)
    glm::vec4 biomeFogForest;       // rgb tint w=density multiplier (>1, denser wood air)
    glm::vec4 biomeFogMountain;     // rgb tint w=density multiplier (<1, clear near air)
    glm::vec4 biomeMountainExtra;   // x=aerialPerspectiveStrength (y/z/w unused)
    // Lighting overhaul: atmosphere-driven direct light + sky-cubemap IBL +
    // ray-traced volumetrics. Appended (never inserted), per this struct's
    // established rule. sunRadiance is computed on the CPU each frame by
    // atmSunTransmittanceCpu() (the mirror of skyModel.glsl's Chapman
    // transmittance) -- sun by day, moon after the twilight handoff.
    glm::vec4 sunRadiance;      // rgb = active direct-light radiance; w = true sun elevation
    glm::vec4 iblParams;        // x=diffuse mip y=max spec mip z=spec intensity w=terrain-only sky-reflect intensity
    glm::vec4 volumetricParams; // x=intensity y=HG anisotropy z=max dist w=steps
    // More editor control over the god-ray look, decoupled from the surface
    // height-fog dial they used to silently inherit from (a user cranking
    // ray density had no way to do that without also fogging the whole
    // scene). Appended, per this struct's established rule.
    glm::vec4 volumetricParams2; // x=densityScale y=heightFalloffScale z=turbulence strength w=wind speed
    // Artistic color override for the scattered light -- lets the rays read
    // as a warm golden shaft even when the physical sun/moon radiance
    // wouldn't produce one. tintStrength=0 leaves the physical color
    // untouched (mix(1,tint,0) == 1), so existing scenes look unchanged
    // until a user opts in.
    glm::vec4 volumetricTint; // rgb=tint color w=tintStrength (0=physical, 1=full tint)
    glm::vec4 stylePaint0;
    glm::vec4 stylePaint1;
    glm::vec4 stylePost0;
    glm::vec4 stylePost1;
    glm::vec4 styleOutlineColor;
    glm::vec4 styleSplitShadow;
    glm::vec4 styleSplitHighlight;
    glm::vec4 styleSky0;
    glm::vec4 styleSkyZenith;
    glm::vec4 styleSkyHorizon;
    glm::vec4 styleCloud0;
    glm::vec4 styleCloudLit;
    glm::vec4 styleCloudMid;
    glm::vec4 styleCloudBase;
    std::array<glm::vec4, 5> terrainPaintLit;
    std::array<glm::vec4, 5> terrainPaintShade;
    std::array<glm::vec4, 4> pointLightPositionRadius;
    std::array<glm::vec4, 4> pointLightColorIntensity;
    glm::vec4 pointLightParams; // x=count; y/z/w reserved
    // Fog redesign (shaders/vulkan/fog.glsl). Appended, per this struct's
    // established rule. fogParams/fogDayColor/fogNightColor above keep their
    // slots; what changed is that fogParams.z is now a transmittance floor
    // instead of a blend ceiling, and the two colors are finally read by a
    // shader.
    glm::vec4 fogParams2; // x=aerialStrength y=sunInscatterStrength z=noiseStrength w=noiseScale
    glm::vec4 fogParams3; // x=groundAnisotropy y=noiseWindSpeed z=skyFogStrength w=unused
    // Cloud deck geometry + density (sky.frag).
    glm::vec4 styleCloud1; // x=deckHeight y=featureScale z=opticalDensity w=sunOcclusion
    // Water (water.frag).
    glm::vec4 waterParams0;  // x=level y=clarity z=roughness w=reflectionStrength
    glm::vec4 waterParams1;  // x=waveAmplitude y=waveScale z=waveSpeed w=ssrSteps
    glm::vec4 waterParams2;  // xy=waveDirection z=ssrThickness w=foamDepth
    glm::vec4 waterParams3;  // x=foamStrength (y/z/w reserved)
    glm::vec4 waterShallowColor; // rgb
    glm::vec4 waterDeepColor;    // rgb
    // Volumetric cloudscape (clouds.frag). styleCloud0/1 above stay shared
    // with the analytic deck that now only feeds the environment cubemap.
    glm::vec4 styleCloud2; // x=layerThickness y=shapeScale z=detailScale w=weatherScale
    glm::vec4 styleCloud3; // x=densityMultiplier y=lightAbsorption z=ambientStrength w=curlStrength
    glm::vec4 styleCloud4; // x=phaseG y=silverIntensity z=silverSpread w=powderStrength
    glm::vec4 styleCloud5; // x=maxMarchDist y=maxSteps z=lightTaps w=cloudTypeBias
    glm::vec4 styleCloud6; // x=detailStrength (y/z/w reserved)
  };
  // The GLSL mirror is shaders/vulkan/frameData.glsl; every scene shader
  // includes that whole block (no per-shader prefixes or offset hacks). If
  // one of these fires, frameData.glsl needs the same layout change.
  static_assert(offsetof(FrameDataGpu, terrainColorSand) == 240,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, camPosWS) == 304,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, skyAmbientParams) == 320,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, terrainMat3) == 336,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, miscParams) == 352,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, terrainTexA) == 368,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, terrainTiling0) == 432,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, biomeAmbientMeadow) == 464,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, biomeMountainExtra) == 560,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, sunRadiance) == 576,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, volumetricParams) == 608,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, volumetricParams2) == 624,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, volumetricTint) == 640,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, stylePaint0) == 656,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, stylePaint1) == 672,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, terrainPaintLit) == 880,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, terrainPaintShade) == 960,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, pointLightPositionRadius) == 1040,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, fogParams2) == 1184,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, styleCloud1) == 1216,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, waterParams0) == 1232,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(offsetof(FrameDataGpu, styleCloud2) == 1328,
                "FrameDataGpu layout drifted from frameData.glsl");
  static_assert(sizeof(FrameDataGpu) == 1408,
                "FrameDataGpu layout drifted from frameData.glsl");

  // Material flag bits (mesh.frag mirrors these exactly).
  enum DrawItemMaterialFlags : uint32_t {
    kHasRoughnessMap = 1u << 0,
    kHasMetallicMap = 1u << 1,
    kHasAOMap = 1u << 2,
    kRoughnessIsGloss = 1u << 3,
    // 2 bits each for roughness/metallic/AO channel selectors (0=R 1=G 2=B 3=A).
    kRoughnessChannelShift = 4,
    kMetallicChannelShift = 6,
    kAOChannelShift = 8,
    kHasNormalMap = 1u << 10,
    kHasOpacityMap = 1u << 11,
    kOpacityChannelShift = 12,
  };

  struct DrawItem {
    uint32_t indexOffset;
    uint32_t indexCount;
    uint32_t textureIndex;
    uint32_t roughnessIndex = 0;
    uint32_t metallicIndex = 0;
    uint32_t aoIndex = 0;
    uint32_t normalIndex = 0;
    uint32_t opacityIndex = 0;
    float roughnessScalar = 0.8f;
    float metallicScalar = 0.0f;
    float aoScalar = 1.0f;
    float alphaCutoff = 0.0f;
    glm::vec3 emissiveColor = glm::vec3(0.0f);
    float emissiveStrength = 0.0f;
    uint32_t materialFlags = 0;
  };

  // A loaded mesh: device-local geometry + its per-material submesh draws.
  // A null vertexBuffer marks a dead (evicted, reusable) slot -- draw loops
  // and TLAS assembly must skip those.
  struct Mesh {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VmaAllocation indexAlloc = VK_NULL_HANDLE;
    std::vector<DrawItem> drawItems;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    // Local-space AABB (computed at build) for frustum culling; hasBounds
    // false means "always draw" (defensive for empty/legacy paths).
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    bool hasBounds = false;
  };

  // A placed object: which mesh + its world transform.
  struct Instance {
    uint32_t meshIndex;
    glm::mat4 model;
    bool isTerrain = false; // routes to mTerrainChunkPipeline instead of mScenePipeline
  };

  bool createCommandPool();
  bool createDescriptorsAndFrameData();
  bool createSampler();
  bool createSceneTargets();
  void destroySceneTargets();
  bool createTlasDescriptors();
  void writeTlasDescriptors();
  bool createTonemapResources();
  void updateTonemapSets();
  bool createSSAOResources();
  void updateSSAOSets();
  bool createBloomResources();
  void updateBloomSets();
  // Sky environment cubemap (IBL) + volumetric-scattering resources. The
  // env map is fixed-size (128^2 x 6, full mip chain) so it is created once
  // in init(); the volumetric scatter target is half-swapchain-res and
  // lives in createSceneTargets()/destroySceneTargets() like SSAO/bloom.
  bool createEnvMapResources();
  void destroyEnvMapResources();
  void updateVolumetricSets();
  bool createScenePipeline(const std::string &shaderDir);
  bool createTerrainChunkPipeline(const std::string &shaderDir);
  bool createVegetationPipeline(const std::string &shaderDir);
  bool createDepthPrepassPipelines(const std::string &shaderDir);
  bool createSkyPipeline(const std::string &shaderDir);
  bool createEnvMapPipeline(const std::string &shaderDir);
  bool createVolumetricPipelines(const std::string &shaderDir);
  bool createWaterPipeline(const std::string &shaderDir);
  // Volumetric clouds. The noise volumes come from assets/clouds/ and are
  // immutable, so they are loaded once in init(); the half-res march target
  // is swapchain-derived and lives in createSceneTargets()/destroy pair.
  bool createCloudNoiseResources(const std::string &assetDir);
  void destroyCloudNoiseResources();
  void updateCloudSets();
  bool createCloudPipelines(const std::string &shaderDir);

public:
  // Uploads the water-height field. `heights` is resolution^2 floats in row-
  // major XZ order covering [origin, origin+worldSize]^2; each value is the
  // water surface altitude there (the global sea level where there is no
  // lake, so bilinear filtering across a shoreline ramps between two real
  // water heights rather than into a sentinel). Safe to call every frame:
  // reallocates only when the resolution changes.
  void setWaterHeightField(const float *heights, uint32_t resolution,
                           glm::vec2 worldOrigin, float worldSize);

  // A texture for UI use (ImGui), returned as the view+sampler pair the
  // ImGui Vulkan backend wants -- the renderer deliberately does not know
  // about ImGui, so the caller does the ImGui_ImplVulkan_AddTexture() step.
  // Owned by the renderer and destroyed with it; intended for a handful of
  // long-lived images (the world map), not per-frame churn.
  struct UiTexture {
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
  };
  UiTexture createUiTexture(const uint8_t *rgba, uint32_t w, uint32_t h);

private:
  bool createSSAOPipeline(const std::string &shaderDir);
  bool createBlurPipeline(const std::string &shaderDir);
  bool createBloomExtractPipeline(const std::string &shaderDir);
  bool createBloomBlurPipeline(const std::string &shaderDir);
  bool createLinePipeline(const std::string &shaderDir);
  bool createTonemapPipeline(const std::string &shaderDir);
  bool loadMeshFromObj(const std::string &path, Mesh &outMesh);
  // Shared by createMeshFromData()/updateMeshFromData(): flattens `data`
  // into `outMesh`'s vertex/index buffers + drawItems. Does not touch
  // mMeshes or any BLAS -- callers decide whether that's a new entry or an
  // in-place replacement. Returns false if `data` has no geometry.
  bool buildMeshFromData(const ::MeshData &data, const std::string &debugName,
                        Mesh &outMesh);
  bool createSyncObjects();
  void recreateSwapchain();

  // Resolved bindless indices for Params::terrainMaterialSlots -- cache,
  // not authored data (rebuilt from the paths whenever they change).
  // UINT32_MAX means "no map" (shader must fall back), distinct from
  // mDefaultTexIndex (a valid, visible checkerboard) so a missing normal/
  // roughness map doesn't silently render as bogus checkerboard data.
  struct TerrainMaterialResolved {
    uint32_t albedoTex = UINT32_MAX;
    uint32_t normalTex = UINT32_MAX;
    uint32_t roughnessTex = UINT32_MAX;
  };
  std::array<TerrainMaterialResolved, 5> mTerrainMaterialTex;
  // Called once per frame from drawFrame() before the FrameDataGpu packing
  // below; a cheap `if (!dirty) return` unless Params::terrainMaterialSlots
  // just changed (editor "Reload Textures" button, or first frame).
  void resolveTerrainMaterialsIfDirty();

  // R3: raw (this-frame) and exponentially-smoothed camera biome weights,
  // the latter feeding the tonemap pass's camera grade. Smoothing state
  // (not authored data, hence not in Params) plus its own delta-time
  // tracking since drawFrame() has no dt parameter otherwise.
  glm::vec3 mCameraBiomeWeightsRaw{1.0f, 0.0f, 0.0f};
  glm::vec3 mCameraBiomeWeightsSmoothed{1.0f, 0.0f, 0.0f};
  std::chrono::steady_clock::time_point mLastGradeUpdate{};
  bool mHasLastGradeUpdate = false;

  uint32_t addTexture(const uint8_t *rgba, uint32_t w, uint32_t h,
                      VkFormat format);
  // flipY mirrors the GL engine's texture loading for raw (unflipped) model
  // UVs; the OBJ smoke path pre-flips UVs instead and loads unflipped.
  // srgb=false for linear (non-color) data: roughness/metallic/AO maps must
  // not go through an sRGB->linear decode the way albedo/diffuse does.
  uint32_t loadTextureFile(const std::string &path, bool flipY = false,
                           bool srgb = true);
  uint32_t createDefaultTexture();

  VkShaderModule loadShaderModule(const std::string &path);
  void immediateSubmit(const std::function<void(VkCommandBuffer)> &record);
  void createDeviceLocalBuffer(const void *data, VkDeviceSize size,
                               VkBufferUsageFlags usage, VkBuffer &outBuffer,
                               VmaAllocation &outAlloc);
  void flushPendingCopiesImmediate();
  MeshHandle acquireMeshSlot(Mesh &&mesh);
  VulkanAccel::BlasInput blasInputForMesh(const Mesh &mesh) const;

  VulkanContext *mCtx = nullptr;
  VkSurfaceKHR mSurface = VK_NULL_HANDLE;
  VulkanSwapchain mSwapchain;
  std::function<void(uint32_t &, uint32_t &)> mQueryFbSize;

  // --- foundations ---
  VulkanBindless mBindless;
  VulkanPipelineCache mPipelineCache;
  VkDescriptorSetLayout mFrameSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool mFrameDescPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> mFrameSets;
  std::vector<VkBuffer> mFrameUBOs;
  std::vector<VmaAllocation> mFrameUBOAllocs;
  std::vector<void *> mFrameUBOMapped;

  // --- textures (bindless) ---
  VkSampler mSampler = VK_NULL_HANDLE;
  std::vector<VkImage> mTextureImages;
  std::vector<VmaAllocation> mTextureAllocs;
  std::vector<VkImageView> mTextureViews;
  uint32_t mDefaultTexIndex = 0;

  // --- offscreen scene targets (per frame in flight) ---
  VkFormat mHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  std::vector<VkImage> mHdrImages;
  std::vector<VmaAllocation> mHdrAllocs;
  std::vector<VkImageView> mHdrViews;
  VkFormat mDepthFormat = VK_FORMAT_D32_SFLOAT;
  std::vector<VkImage> mDepthImages;
  std::vector<VmaAllocation> mDepthAllocs;
  std::vector<VkImageView> mDepthViews;

  // --- SSAO (raw + blurred, per frame in flight) ---
  VkFormat mAOFormat = VK_FORMAT_R8_UNORM;
  std::vector<VkImage> mSSAOImages, mSSAOBlurImages;
  std::vector<VmaAllocation> mSSAOAllocs, mSSAOBlurAllocs;
  std::vector<VkImageView> mSSAOViews, mSSAOBlurViews;
  // Three single-binding sampler descriptor sets (mirrors the tonemap
  // pattern): SSAO pass samples depth, blur pass samples raw AO, scene pass
  // samples blurred AO.
  VkDescriptorSetLayout mAOSamplerSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool mAOSamplerPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> mSSAODepthSets;    // sample mDepthViews
  std::vector<VkDescriptorSet> mBlurInputSets;    // sample mSSAOViews
  std::vector<VkDescriptorSet> mSceneAOSets;      // sample mSSAOBlurViews
  VkPipelineLayout mSSAOPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mSSAOPipeline = VK_NULL_HANDLE;
  VkPipelineLayout mBlurPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mBlurPipeline = VK_NULL_HANDLE;
  // Depth-only prepass: same vertex stage and mScenePipelineLayout as the
  // main scene pipeline (and terrain chunks, which share that vertex
  // layout), just no fragment shader/color output.
  VkPipeline mDepthPrepassPipeline = VK_NULL_HANDLE;
  // Alpha-tested variants use the same vertices/depth state but run a tiny
  // fragment shader so cutout holes do not poison SSAO and early-Z.
  VkPipeline mDepthAlphaPrepassPipeline = VK_NULL_HANDLE;
  // Depth-only prepass for GPU-instanced vegetation: meshInstanced.vert
  // (2nd vertex binding for the per-instance model matrix), no fragment
  // shader/color output. A separate pipeline from mDepthPrepassPipeline
  // because the vertex input state (bindings/attributes) differs.
  VkPipeline mVegetationDepthPrepassPipeline = VK_NULL_HANDLE;
  VkPipeline mVegetationDepthAlphaPrepassPipeline = VK_NULL_HANDLE;

  // --- Bloom: half-res bright-pass extract + 2-pass separable blur, plus a
  // second quarter-res blur pass (downsampled from the half-res result, same
  // mBloomBlurPipeline reused again) for a wider, softer glow -- summed with
  // the half-res "tight" glow in tonemap.frag. Two fixed scales rather than
  // a full mip chain (not worth the extra passes at this scene scale). ---
  std::vector<VkImage> mBloomExtractImages, mBloomBlurHImages, mBloomBlurVImages;
  std::vector<VmaAllocation> mBloomExtractAllocs, mBloomBlurHAllocs,
      mBloomBlurVAllocs;
  std::vector<VkImageView> mBloomExtractViews, mBloomBlurHViews,
      mBloomBlurVViews;
  std::vector<VkImage> mBloomBlurQ2HImages, mBloomBlurQ2VImages;
  std::vector<VmaAllocation> mBloomBlurQ2HAllocs, mBloomBlurQ2VAllocs;
  std::vector<VkImageView> mBloomBlurQ2HViews, mBloomBlurQ2VViews;
  // Reuses mAOSamplerSetLayout (one combined-image-sampler at binding 0,
  // fragment stage) -- same shape, just a separate pool/sets.
  VkDescriptorPool mBloomSamplerPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> mBloomExtractInputSets; // sample mHdrViews
  std::vector<VkDescriptorSet> mBloomBlurHInputSets;   // sample mBloomExtractViews
  std::vector<VkDescriptorSet> mBloomBlurVInputSets;   // sample mBloomBlurHViews
  std::vector<VkDescriptorSet> mBloomBlurQ2HInputSets; // sample mBloomBlurVViews (downsample)
  std::vector<VkDescriptorSet> mBloomBlurQ2VInputSets; // sample mBloomBlurQ2HViews
  VkPipelineLayout mBloomExtractPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mBloomExtractPipeline = VK_NULL_HANDLE;
  VkPipelineLayout mBloomBlurPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mBloomBlurPipeline = VK_NULL_HANDLE; // reused for H, V, and the quarter-res H/V passes

  // --- ray tracing: acceleration structures + TLAS descriptor (set 2) ---
  VulkanAccel mAccel;
  VkDescriptorSetLayout mTlasSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool mTlasPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> mTlasSets; // per frame in flight (same TLAS)

  // --- scene (mesh) pipeline ---
  VkPipelineLayout mScenePipelineLayout = VK_NULL_HANDLE;
  VkPipeline mScenePipeline = VK_NULL_HANDLE;

  // --- procedural sky (fullscreen, push-constant only) ---
  VkPipelineLayout mSkyPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mSkyPipeline = VK_NULL_HANDLE;

  // --- sky environment cubemap (per-frame IBL): 6 faces rendered with the
  // sky shader each frame (sun disc/stars zeroed), then a blit mip chain;
  // scene shaders sample it as set 4 for diffuse irradiance (deep mip) and
  // roughness-mapped specular. Per frame in flight so frame N+1's re-render
  // never races frame N's sampling. ---
  static constexpr uint32_t kEnvFaceSize = 128;
  static constexpr uint32_t kEnvMipCount = 8; // 128 -> 1
  std::vector<VkImage> mEnvImages;
  std::vector<VmaAllocation> mEnvAllocs;
  std::vector<VkImageView> mEnvCubeViews;               // cube view, all mips
  std::vector<std::array<VkImageView, 6>> mEnvFaceViews; // 2D views, mip 0
  VkDescriptorPool mEnvVolPool = VK_NULL_HANDLE; // env + volumetric sampler sets
  std::vector<VkDescriptorSet> mSceneEnvSets;    // sample mEnvCubeViews (set 4)
  VkPipeline mEnvMapPipeline = VK_NULL_HANDLE;   // sky variant: no depth attachment

  // --- ray-traced volumetric scattering (god rays): half-res march target +
  // additive composite into HDR. Reuses mAOSamplerSetLayout shapes and the
  // SSAO depth sets. ---
  std::vector<VkImage> mVolumetricImages;
  std::vector<VmaAllocation> mVolumetricAllocs;
  std::vector<VkImageView> mVolumetricViews;
  std::vector<VkDescriptorSet> mVolScatterSets; // sample mVolumetricViews
  VkPipelineLayout mVolumetricPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mVolumetricPipeline = VK_NULL_HANDLE;
  VkPipelineLayout mVolCompositePipelineLayout = VK_NULL_HANDLE;
  VkPipeline mVolCompositePipeline = VK_NULL_HANDLE;

  // --- volumetric cloudscape (shaders/vulkan/clouds.frag) ---
  // Half-res raymarch of a Nubis-style cloud layer, composited over the sky
  // inside the scene pass before any geometry draws. The two 128^3 noise
  // volumes and the two 2D maps are immutable, loaded once from
  // assets/clouds/, and live in a single descriptor set (not per frame in
  // flight -- nothing writes them after upload).
  VkImage mCloudShapeImage = VK_NULL_HANDLE;   // Perlin-Worley base shape
  VmaAllocation mCloudShapeAlloc = VK_NULL_HANDLE;
  VkImageView mCloudShapeView = VK_NULL_HANDLE;
  VkImage mCloudDetailImage = VK_NULL_HANDLE;  // Nubis 4-channel detail
  VmaAllocation mCloudDetailAlloc = VK_NULL_HANDLE;
  VkImageView mCloudDetailView = VK_NULL_HANDLE;
  VkImage mCloudWeatherImage = VK_NULL_HANDLE; // r=coverage g=wetness b=type
  VmaAllocation mCloudWeatherAlloc = VK_NULL_HANDLE;
  VkImageView mCloudWeatherView = VK_NULL_HANDLE;
  VkImage mCloudCurlImage = VK_NULL_HANDLE;
  VmaAllocation mCloudCurlAlloc = VK_NULL_HANDLE;
  VkImageView mCloudCurlView = VK_NULL_HANDLE;
  VkSampler mCloudSampler = VK_NULL_HANDLE; // REPEAT + full mip range
  VkDescriptorSetLayout mCloudNoiseSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool mCloudPool = VK_NULL_HANDLE;
  VkDescriptorSet mCloudNoiseSet = VK_NULL_HANDLE;
  bool mCloudNoiseReady = false; // false if the assets failed to load
  std::vector<VkImage> mCloudImages;
  std::vector<VmaAllocation> mCloudAllocs;
  std::vector<VkImageView> mCloudViews;
  std::vector<VkDescriptorSet> mCloudSampleSets; // composite samples mCloudViews
  VkPipelineLayout mCloudPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mCloudPipeline = VK_NULL_HANDLE;
  VkExtent2D mCloudExtent{1, 1};

  // --- water (shaders/vulkan/water.frag) ---
  // The water surface is not geometry. It is an analytic plane at
  // Params::waterLevel, intersected with the view ray in a fullscreen pass
  // that runs after the opaque scene. That buys an unbounded surface with no
  // tessellation, no LOD seams and no chunk streaming, which for a single
  // world-height water table is all upside; the cost is that it cannot model
  // lakes at differing heights (see Params::waterLevel).
  //
  // mSceneCopy* is a full-res snapshot of the opaque HDR image taken just
  // before the water pass. Water needs to READ the scene it is covering --
  // once for refraction through the surface, and again as the colour source
  // for the screen-space reflection march -- and it cannot sample the render
  // target it is writing to.
  std::vector<VkImage> mSceneCopyImages;
  std::vector<VmaAllocation> mSceneCopyAllocs;
  std::vector<VkImageView> mSceneCopyViews;
  std::vector<VkDescriptorSet> mSceneCopySets; // sample mSceneCopyViews
  VkPipelineLayout mWaterPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mWaterPipeline = VK_NULL_HANDLE;

  // Water-height field: a world-space grid of water surface altitudes around
  // the camera, so the water pass can shade lakes at their own levels instead
  // of one global plane. Sampled from Engine/Terrain's waterSurfaceAt() -- the
  // CPU stays the single authority for where water is, as it already is for
  // where the ground is. Outside the field's footprint the shader falls back
  // to the global sea level, which is what makes an ocean reach the horizon
  // while the field only has to cover the streamed disc.
  // Long-lived UI images (see createUiTexture).
  std::vector<VkImage> mUiImages;
  std::vector<VmaAllocation> mUiAllocs;
  std::vector<VkImageView> mUiViews;

  VkImage mWaterFieldImage = VK_NULL_HANDLE;
  VmaAllocation mWaterFieldAlloc = VK_NULL_HANDLE;
  VkImageView mWaterFieldView = VK_NULL_HANDLE;
  VkDescriptorPool mWaterFieldPool = VK_NULL_HANDLE;
  VkDescriptorSet mWaterFieldSet = VK_NULL_HANDLE;
  uint32_t mWaterFieldResolution = 0;
  glm::vec2 mWaterFieldOrigin{0.0f}; // world XZ of texel (0,0)
  float mWaterFieldSize = 0.0f;      // world metres covered per side

  // --- terrain chunk pipeline; a regular indexed mesh pipeline (like the
  // scene pipeline) reusing mScenePipelineLayout, just a different
  // fragment shader (terrainChunk.frag's height/slope material blend).
  // Terrain chunks are built on the CPU (Engine/Terrain/) and rendered as
  // ordinary instances -- see Instance::isTerrain.
  VkPipeline mTerrainChunkPipeline = VK_NULL_HANDLE;

  // --- vegetation (GPU-instanced) pipeline; reuses mScenePipelineLayout and
  // mesh.frag, but meshInstanced.vert (a 2nd vertex binding carries the
  // per-instance model matrix instead of the push constant) -- see
  // setVegetationBatches(). ---
  VkPipeline mVegetationPipeline = VK_NULL_HANDLE;
  struct VegSpeciesBuffer {
    MeshHandle mesh = UINT32_MAX;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    void *mapped = nullptr;
    size_t capacity = 0; // instances
    uint32_t count = 0;
    // CPU mirror of the instance data: TLAS assembly reads transforms from
    // here instead of the write-combined mapped buffer (uncached CPU reads
    // from that memory are extremely slow).
    std::vector<VegInstanceGpu> cpu;
    // Per-source-chunk grouping for frustum/distance culling. For layers
    // with ScatterLayer::cullCellSize set (grass), these are finer
    // sub-chunk cells instead -- see VegBatch::ranges.
    std::vector<VegRange> ranges;
    // Mirrors the same-named VegBatch fields; see their comments.
    float drawDistance = 0.0f;
    bool rayTracedShadows = true;
    float windStrength = 0.0f;
    float windSpeed = 1.0f;
    float windMeshHeight = 1.0f;
    float groundOcclusion = 0.0f;
    float foliageSssStrength = 1.0f;
    // Per-frame scratch: merged contiguous [first,count) spans of visible
    // ranges, rebuilt by drawFrame() and drawn by both depth prepass and
    // scene pass.
    std::vector<std::pair<uint32_t, uint32_t>> visibleSpans;
  };
  // One entry per species mesh with any placed instances. Grown (buffer
  // recreated larger) like a std::vector on overflow; never shrunk.
  std::vector<VegSpeciesBuffer> mVegBuffers;

  // --- editor debug lines (grid, axes, outlines) ---
  // No descriptor sets: viewProj and the fade parameters ride in a push
  // constant, so this pipeline can be recorded mid-scene-pass without
  // disturbing any binding the mesh/terrain draws set up.
  struct LinePush {
    glm::mat4 viewProj;
    glm::vec4 camPosFade;
  };
  VkPipelineLayout mLinePipelineLayout = VK_NULL_HANDLE;
  VkPipeline mLinePipeline = VK_NULL_HANDLE;
  // Host-visible and grown like a std::vector; the line set is rebuilt from
  // scratch every frame (the grid follows the camera), so a staged upload
  // would cost more than it saves.
  std::vector<VkBuffer> mLineBuffers;
  std::vector<VmaAllocation> mLineAllocs;
  std::vector<void *> mLineMapped;
  std::vector<size_t> mLineCapacity; // vertices, per frame in flight
  std::vector<DebugLineVertex> mDebugLines;
  uint32_t mDebugLineCount = 0;
  float mDebugLineFade = 220.0f;

  // --- tonemap pipeline ---
  VkDescriptorSetLayout mTonemapSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool mTonemapPool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> mTonemapSets;
  VkPipelineLayout mTonemapPipelineLayout = VK_NULL_HANDLE;
  VkPipeline mTonemapPipeline = VK_NULL_HANDLE;

  // --- mesh (device-local) ---
  std::vector<Mesh> mMeshes;
  std::vector<Instance> mInstances;
  // Dead mMeshes indices available for reuse (mesh handle == BLAS slot must
  // stay stable, so eviction never erases from the vector).
  std::vector<uint32_t> mFreeMeshSlots;
  // Per-frame frustum-visibility scratch, parallel to mInstances (shared by
  // the depth prepass and scene pass so the test runs once per instance).
  std::vector<uint8_t> mInstanceVisible;

  // --- streamed upload batching + deferred destruction ---
  // While the scene is live, createDeviceLocalBuffer() queues its staging->
  // device copy here instead of doing a blocking submit per buffer;
  // drawFrame() records every queued copy (and queued BLAS build) into the
  // frame's own command buffer behind one pair of barriers.
  struct PendingCopy {
    VkBuffer src = VK_NULL_HANDLE;
    VkBuffer dst = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
  };
  std::vector<PendingCopy> mPendingCopies;
  std::vector<std::pair<uint32_t, VulkanAccel::BlasInput>> mPendingBlasBuilds;
  // Resources retired this frame (staging buffers, replaced mesh buffers,
  // released BLAS) -- moved into mFrameGarbage[frame] at the next
  // drawFrame() and freed once that frame's fence has signaled again.
  VulkanAccel::PendingGarbage mPendingGarbage;
  std::array<VulkanAccel::PendingGarbage, kFramesInFlight> mFrameGarbage;

  // --- TLAS change tracking (rebuild only when the instance set changes) ---
  std::vector<VulkanAccel::InstanceInput> mTlasSceneInputs;     // this frame's mInstances section (scratch)
  std::vector<VulkanAccel::InstanceInput> mTlasSceneInputsPrev; // last built section, for change detection
  std::vector<VulkanAccel::InstanceInput> mTlasVegInputs;       // cached near-camera veg section
  std::vector<VulkanAccel::InstanceInput> mTlasBuildInputs;     // scene + veg, what stale slots rebuild from
  std::array<bool, kFramesInFlight> mTlasSlotStale{};
  void markTlasAllStale() { mTlasSlotStale.fill(true); }
  uint64_t mVegGeneration = 0;          // bumped by setVegetationBatches()
  uint64_t mVegTlasGeneration = ~0ull;  // generation mTlasVegInputs was built at
  glm::vec3 mVegTlasCamPos{0.0f};
  bool mHasVegTlasCamPos = false;

  // Solid-color 1x1 texture cache (packed RGBA8 -> bindless index), shared
  // across meshes -- without it every streamed terrain chunk minted its own
  // white texture into the bindless array.
  std::unordered_map<uint32_t, uint32_t> mSolidColorTex;

  FrameStats mFrameStats;

  // --- frame loop ---
  VkCommandPool mCommandPool = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> mCommandBuffers;
  std::vector<VkSemaphore> mImageAvailable;
  std::vector<VkFence> mInFlight;
  std::vector<VkSemaphore> mRenderFinished;

  uint32_t mCurrentFrame = 0;
  // Monotonic, unlike mCurrentFrame (which only cycles over the frames in
  // flight). The cloud march rotates its dither phase off this so the step
  // lattice reads as noise the eye integrates away rather than a static grain
  // locked to the screen.
  uint64_t mFrameCounter = 0;
  std::chrono::steady_clock::time_point mStartTime;

  bool mCapture = false;
  std::string mCapturePath;
  uint32_t mCaptureMaxDim = 0; // 0 = write at native resolution

  Params mParams;
  std::function<void(VkCommandBuffer)> mOverlay;
  bool mSceneReady = false;
};

} // namespace vkrhi
