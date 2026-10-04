#pragma once
// TerrainSettings.h — tunable parameters for the noise/height/biome pipeline
// and (for forward compat with later phases) streaming/collision/vegetation
// budgets.
//
// R1 (MEADOW_TERRAIN_REVAMP_PLAN.md): singleBiomeOnly/biomeScale/seaLevel
// (the old 6-biome/ocean machinery) are deleted. chunkResolution goes from
// 33 to 129 (2m/vertex -> 0.5m/vertex at LOD0) -- the single biggest fix for
// the "blobby low-poly terrain" complaint; no shader work can fix a
// silhouette this coarse. New fields drive the 3-biome landform/weight
// field (meadow/forest/mountain) in TerrainNoise.cpp.

#include <cstdint>

struct TerrainSettings {
  // Fixed pond / marsh / woodland layout, independent of the noise seed.
  bool authoredWoodland = false;
  uint32_t seed = 1337;
  float chunkWorldSize = 64.0f;
  uint32_t chunkResolution = 129;

  float heightScale = 18.0f;
  // Metres subtracted from every sampled height. Terrain is built from a sum
  // of non-negative noise terms, so without this it can only ever sit ABOVE
  // y=0 -- typically 1-2x heightScale up, which buries the world origin and
  // the editor grid underground. VkTerrainSubsystem::create() measures the
  // generated mean and sets this so the terrain straddles y=0 instead.
  float heightOffset = 0.0f;
  // Measure and apply heightOffset automatically at creation. Turn off to
  // pin terrain at an explicit absolute height.
  bool autoCenterHeight = true;
  float noiseFrequency = 0.01f;
  int octaves = 5;
  float lacunarity = 2.0f;
  float gain = 0.5f;

  float macroStrength = 1.0f;
  float landscapeScale = 2.2f;   // ported from old system; larger = broader macro landforms
  float valleySpan = 1.25f;      // ported from old system; broader/tighter valley basins
  bool useRidgeNoise = false;    // ported from old system; ridgeShape uses ridgeNoise() vs abs(fbm())
  float ridgeBlend = 1.0f;       // 0 = rounded abs(fbm), 1 = full sharp ridge noise

  // --- R1: landform & 3-biome field (Engine/Terrain/TerrainNoise.cpp) ---
  // Mountain-region macro mask: one very-low-frequency field deciding where
  // the world is mountain country vs. lowland; multiplies relief amplitude
  // (mountainHeightScale) and drives wMountain directly (plan §4b/§4b').
  float mountainRegionScale = 1.0f;  // 1/wavelength multiplier for the mask
  float mountainCoverage = 0.32f;    // 0..1, how much of the map is mountain country
  float mountainHeightScale = 3.2f;  // relief amplitude multiplier inside mountain regions
  float valleyDepth = 1.0f;          // strength of valley-floor carving (macroValley)
  float erosionStrength = 0.6f;      // derivative-damped ("eroded") fbm strength (0 = plain fbm)
  float microReliefStrength = 1.0f;  // 0..1 scale of the 0.3-1m bump/hollow layer
  float outcropThreshold = 0.55f;    // slope (0..1, 1=vertical) where rock starts breaking through turf

  // Forest coverage/treeline -- feeds wForest alongside moisture and
  // altitude; mountains get their treeline "for free" from this (forest
  // weight already fades with mountainMask, this just adds the hard
  // altitude cutoff so peaks are always bare).
  float forestCoverage = 0.45f;      // 0..1, how much of non-mountain land is forest
  float treelineHeight = 30.0f;      // world-height meters where trees stop
  float treelineTransition = 8.0f;   // meters over which trees fade out approaching treeline

  // --- World shape: a bounded map of islands, or endless terrain ---
  //
  // The generator streams chunks around the camera either way -- bounding the
  // world does NOT mean loading it all at once, and could not: a 4 km radius
  // at 64 m chunks is ~12,000 chunks. What `worldBounded` changes is the
  // SHAPE: a continent mask carves the land into discrete islands separated
  // by real ocean, and a radial falloff sinks everything past worldRadius, so
  // the map has an outside. Turn it off for the original endless noise field.
  bool worldBounded = true;
  float worldRadius = 4000.0f;     // metres from origin to open ocean
  float worldEdgeFalloff = 1100.0f; // metres over which the rim sinks
  // Metres per continent feature. This is the single most character-defining
  // number here: small values give an archipelago of many little islands,
  // large values give a few big landmasses with long coastlines.
  float continentScale = 1500.0f;
  // Fraction of the bounded disc that ends up above water. The continent mask
  // is shaped so 0.5 on its field IS the shoreline, so this reads directly.
  float landCoverage = 0.42f;
  float oceanFloorDepth = 45.0f;  // metres below sea level out in open water
  float landBaseHeight = 14.0f;   // metres above sea level a continent core sits
  // Guaranteed land around the origin, so a spawn is never in open sea.
  // 0 disables it and lets the mask decide.
  float spawnIslandRadius = 300.0f;

  // --- Water (Engine/Terrain/TerrainWater.h) ---
  // Sea level and inland lakes. Both feed one authority, waterSurfaceAt(),
  // which the generator carves against, the scatter system refuses to plant
  // below, and the renderer samples into a height field to shade.
  bool oceanEnabled = true;
  // The ABSOLUTE sea level, in world metres. Resolved at terrain creation
  // from oceanCoverage below unless autoSeaLevel is off -- everything
  // downstream (carving, scatter, the renderer's water field) reads this one
  // number, so there is a single answer to "where is the sea".
  float seaLevel = 0.0f;
  // Fraction of the world the ocean should cover. This is the dial to author
  // with: an absolute sea level means nothing until you know what heightScale,
  // seed and mountainHeightScale did to the terrain, and those interact.
  // (autoCenterHeight/heightOffset, which the comment above them claims
  // recentres the terrain on y=0, are declared but read by nothing -- so the
  // generated range is wherever the noise puts it, typically far above 0.)
  bool autoSeaLevel = true;
  float oceanCoverage = 0.35f;
  // Vertical band around the sea line over which relief is compressed. This
  // is what makes beaches: without it the waterline is an arbitrary contour
  // across ordinary hillside and every coast is a cliff.
  float shoreFlatten = 5.0f;

  bool lakesEnabled = true;
  // Cell size of the lake grid, in metres -- one lake per cell at most, so
  // this is also the closest two lakes can be.
  float lakeScale = 260.0f;
  // Fraction of cells that become lakes, BEFORE the valley bias and the
  // flatness test reject those on slopes. Effective coverage is a good deal
  // lower than this, which is why the number looks high.
  float lakeCoverage = 0.70f;
  // How deep a basin is carved below its surface. Also gates the flatness
  // test: ground varying by more than ~1.35x this across a cell is a
  // hillside, not a basin, and gets no lake.
  float lakeDepth = 7.0f;

  // Metres of dry margin above the waterline that scatter leaves clear, so
  // trees and grass stop short of the shore instead of standing in the
  // shallows.
  float shoreScatterMargin = 0.6f;

  // --- Phase 2+ (streaming) ---
  int viewDistanceChunks = 6;
  uint32_t workerThreads = 4;
  // Per-frame budgets: build-job dispatches (new loads + LOD refreshes,
  // nearest-first) and completed-chunk GPU uploads.
  uint32_t maxChunkLoadsPerUpdate = 8;
  uint32_t maxCompletedChunksPerFrame = 4;

  // --- Phase 4 (collision), inert in Phase 0 ---
  uint32_t collisionChunkRadius = 2, collisionUpdatesPerFrame = 2;

  // --- Phase 3 (vegetation) ---
  // Everything below scales the ScatterManifest's AUTHORED per-layer values
  // rather than replacing them, so the manifest keeps expressing the
  // relationships between layers (pine vs. sapling proportions, meadow vs.
  // tuft grass density) while these sliders move the whole set together.
  // That is what makes them safe to expose as live editor controls: no
  // slider position can invert the artistic intent baked into the manifest.
  bool spawnVegetation = true;

  // --- Trees ---
  // Scales the placement density of Tree-typed scatter layers only.
  // 1.0 = each layer's authored ScatterLayer::density as-is. Defaults below
  // 1.0 because the authored densities read as cluttered at default settings.
  float treeDensityMultiplier = 0.4f;
  // UNIFORM size of every Tree layer -- the control to reach for when trees
  // should be bigger. Scales all three axes, so the asset keeps its authored
  // proportions and a big tree looks like a big tree.
  float treeSizeMultiplier = 1.0f;
  // Vertical-only stretch, on top of the uniform size above. 1.0 (the
  // default) leaves the asset's proportions alone; above ~1.3 trees start
  // reading as a distorted mesh rather than a taller plant, so this is a
  // deliberate stylization dial, not the way to make trees larger. Kept
  // separate from treeSizeMultiplier precisely so the common case (bigger
  // trees) cannot accidentally distort them.
  float treeHeightMultiplier = 1.0f;
  // Widens (>1) or compresses (<1) the SPREAD between a layer's min and max
  // height around its midpoint, leaving the average height alone. 0 makes
  // every tree in a layer identical; high values give a ragged, uneven
  // canopy line.
  float treeHeightVariance = 1.0f;
  // Scales every Tree layer's minSpacing -- "how close together may trees
  // stand". Raising it thins stands into open woodland even at unchanged
  // density (candidates that fail the spacing test are simply dropped), so
  // this and treeDensityMultiplier are genuinely independent controls:
  // density sets how many are ATTEMPTED, spacing sets how many can fit.
  float treeSpacingMultiplier = 1.0f;
  // Scales every layer's clustering.clearingChance: 0 = no clearings at all
  // (continuous forest), high = forest broken into small isolated copses.
  float forestPatchiness = 1.0f;
  // Scales every layer's clustering.standRadius -- the size of one grove.
  float standRadiusMultiplier = 1.0f;
  // Extra random tilt (degrees) added to every Tree layer's leanMaxDeg, for
  // storm-battered / old-growth looks.
  float treeLeanExtraDeg = 0.0f;
  // Chebyshev chunk radius around the camera within which eligible trees
  // are promoted to physically-collidable entities.
  int interactiveTreeChunkRadius = 2;

  // --- Rocks ---
  float rockDensityMultiplier = 1.0f;

  // --- Grass (R5) ---
  bool spawnGrass = true;
  float grassDensityMultiplier = 1.0f;
  // Uniform size of every Grass layer (all three axes), same relationship to
  // grassHeightMultiplier as treeSizeMultiplier has to treeHeightMultiplier.
  float grassSizeMultiplier = 1.0f;
  // Vertical-only stretch, on top of the uniform size. Grass tolerates this
  // far better than trees do -- blades are already long and thin, so
  // stretching reads as a different species rather than as distortion.
  float grassHeightMultiplier = 1.0f;

  // --- Grass shading ---
  // How much darker a grass blade is at its root than at its tip, 0..1
  // (scales each layer's authored ScatterLayer::groundOcclusion). This is
  // the cheap stand-in for the contact shadow grass would cast on itself and
  // on the ground: it costs one multiply in the fragment shader, and without
  // it grass reads as sitting ON the terrain rather than growing OUT of it.
  float grassOcclusionStrength = 1.0f;
  // Patch contact shading supplies grass's ground shadow without putting its
  // blades into the TLAS. Receiving tree/terrain shadows stays independent.
  bool grassCastShadows = false;
  // Scales every Grass layer's maxDrawDistance/densityFalloffStart together,
  // so the falloff ramp keeps its shape. This is the single most effective
  // grass performance control -- cost scales with the square of it.
  float grassDrawDistanceMultiplier = 1.0f;
  // Chebyshev chunk radius around the camera within which Grass layers are
  // scattered at all. A HARD cap, independent of draw distance: scatter runs
  // per chunk on a worker thread and stores its result for as long as the
  // chunk stays loaded, so without this the CPU placement cost and the
  // resident instance memory would both scale with viewDistanceChunks even
  // though nothing past grassDrawDistance can ever be drawn.
  int grassChunkRadius = 2;

  // --- Wind (R5) ---
  // Global multipliers over every layer's authored windStrength/windSpeed.
  float windStrength = 1.0f;
  float windSpeed = 1.0f;
};
