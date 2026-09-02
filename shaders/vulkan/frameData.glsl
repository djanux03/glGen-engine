// Shared per-frame uniform block -- the single GLSL mirror of
// VulkanRenderer's FrameDataGpu struct (static_asserts there pin the byte
// offsets this file implies). Every scene-pass shader includes the WHOLE
// block via GL_GOOGLE_include_directive; never re-declare a prefix of it
// with hand-computed layout(offset=...) -- a missed offset here is exactly
// how the terrain shader once read the camera position as its material
// parameters.
#ifndef FRAME_DATA_SET
#define FRAME_DATA_SET 1
#endif
layout(set = FRAME_DATA_SET, binding = 0) uniform FrameData {
    mat4 viewProj;
    mat4 view;
    vec4 lightDir;      // xyz = direction light travels, w = sun intensity
    vec4 fogParams;     // x=density y=startDist z=maxOpacity w=heightFalloff
    vec4 fogDayColor;   // rgb
    vec4 fogNightColor; // rgb
    vec4 lightParams;   // x=ambient y=shadowStrength z=shadowSoftness w=shadowSamples
    vec4 terrainMat1;   // x=grassStart y=grassEnd z=snowStart w=snowEnd (world-height meters)
    vec4 terrainMat2;   // x=rockSlopeStart y=rockSlopeEnd z=detailScale w=detailStrength
    vec4 terrainColorSand;
    vec4 terrainColorGrass;
    vec4 terrainColorRock;
    vec4 terrainColorSnow;
    vec4 camPosWS;          // xyz = camera world position
    vec4 skyAmbientParams;  // x=nightSkyBrightness y=duskStrength
    vec4 terrainMat3;       // x=biomeTintEnabled(0/1) y=biomeTintIntensity z=macroVariationStrength w=rockDetailStrength
    vec4 miscParams;        // x=debugViewMode(0=off 1=albedo 2=normals 3=fog 4=ssao 5=shadowVis 6=nanDetect 7=biomeWeights) y=fogHeightRef z=timeSeconds
    // R2: 5-layer image splat material (meadow/forest/dirt/rock/scree),
    // bindless indices resolved once from file paths, not per frame.
    uvec4 terrainTexA;      // x=meadowAlbedo y=meadowNormal z=meadowRoughness w=forestAlbedo
    uvec4 terrainTexB;      // x=forestNormal y=forestRoughness z=dirtAlbedo w=dirtNormal
    uvec4 terrainTexC;      // x=dirtRoughness y=rockAlbedo z=rockNormal w=rockRoughness
    uvec4 terrainTexD;      // x=screeAlbedo y=screeNormal z=screeRoughness w=unused
    vec4 terrainTiling0;    // x=meadow y=forest z=dirt w=rock (world units per tile)
    vec4 terrainTiling1;    // x=scree (y/z/w unused)
    // R3: biome lighting & atmosphere (see biomeLighting.glsl).
    vec4 biomeAmbientMeadow;   // rgb tint w=intensity scale
    vec4 biomeAmbientForest;   // rgb tint w=intensity scale
    vec4 biomeAmbientMountain; // rgb tint w=intensity scale
    vec4 biomeDirectParams;    // x=forestCanopyOcclusion y=forestLightShaftStrength z=mountainDirectBoost w=globalStrengthDial
    vec4 biomeFogForest;       // rgb tint w=densityMult (>1)
    vec4 biomeFogMountain;     // rgb tint w=densityMult (<1)
    vec4 biomeMountainExtra;   // x=aerialPerspectiveStrength
    // Lighting overhaul: atmosphere-driven direct light + sky-cubemap IBL +
    // ray-traced volumetrics. sunRadiance.rgb is the ACTIVE direct light's
    // radiance (sun by day, moon after the twilight handoff), already
    // transmittance-colored on the CPU (atmSunTransmittanceCpu) -- direct
    // lighting must use it instead of lightDir.w, which now carries the
    // radiance's luminance for legacy scalar consumers (grass optics).
    vec4 sunRadiance;      // rgb = direct-light radiance; w = TRUE sun elevation (sin)
    vec4 iblParams;        // x=diffuse mip y=max spec mip z=spec intensity w=terrain-only sky-reflect intensity
    vec4 volumetricParams; // x=intensity y=HG anisotropy z=max march dist w=steps
    vec4 volumetricParams2; // x=densityScale y=heightFalloffScale z=turbulence strength w=wind speed
    vec4 volumetricTint;    // rgb=tint color w=tintStrength (0=physical, 1=full tint)
    vec4 stylePaint0;       // mottle strength/scale, world paper, wash edge
    vec4 stylePaint1;       // facet, autumn, foliage SSS boost, reserved
    vec4 stylePost0;        // vibrance, split balance, outline width, outline strength
    vec4 stylePost1;        // depth threshold, normal threshold, max distance, screen paper
    vec4 styleOutlineColor;
    vec4 styleSplitShadow;
    vec4 styleSplitHighlight;
    vec4 styleSky0;         // grade strength, band count, softness, sun softness
    vec4 styleSkyZenith;
    vec4 styleSkyHorizon;
    vec4 styleCloud0;       // coverage, softness, wind x/y
    vec4 styleCloudLit;
    vec4 styleCloudMid;
    vec4 styleCloudBase;
    vec4 terrainPaintLit[5];   // rgb color, mottle scale
    vec4 terrainPaintShade[5]; // rgb color, overlay strength
    vec4 pointLightPositionRadius[4]; // xyz=position w=radius
    vec4 pointLightColorIntensity[4]; // rgb=color w=intensity
    vec4 pointLightParams;            // x=count
    // Fog redesign (fog.glsl). fogParams/fogDayColor/fogNightColor above keep
    // their slots and their meanings, with two exceptions documented in
    // fog.glsl: fogParams.z (maxOpacity) is now a transmittance FLOOR rather
    // than a blend ceiling, and the two colors -- uploaded every frame and
    // read by nothing until now -- tint the ground layer.
    vec4 fogParams2; // x=aerialStrength y=sunInscatterStrength z=noiseStrength w=noiseScale
    vec4 fogParams3; // x=groundAnisotropy y=noiseWindSpeed z=skyFogStrength w=unused
    // Cloud deck geometry + density (sky.frag). styleCloud0 above still holds
    // coverage/softness/wind; these are the dials the old palette-lerp cloud
    // had no use for and the lit one does.
    vec4 styleCloud1; // x=deckHeight(m) y=featureScale(m) z=opticalDensity w=sunOcclusion
    // Water (water.frag). The surface is an analytic plane at waterParams0.x,
    // not geometry -- see VulkanRenderer::Params for why.
    vec4 waterParams0;  // x=level y=clarity z=roughness w=reflectionStrength
    vec4 waterParams1;  // x=waveAmplitude y=waveScale z=waveSpeed w=ssrSteps
    vec4 waterParams2;  // xy=waveDirection z=ssrThickness w=foamDepth
    vec4 waterParams3;  // x=foamStrength
    vec4 waterShallowColor;
    vec4 waterDeepColor;
    // Volumetric cloudscape (clouds.frag). styleCloud0/1 above are SHARED
    // with the analytic deck sky.frag still draws into the environment
    // cubemap -- coverage, softness, wind, layer bottom and sun occlusion
    // mean the same thing to both, so one set of dials drives both. These
    // five are the volumetric marcher's own.
    vec4 styleCloud2; // x=layerThickness(m) y=shapeScale(m) z=detailScale(m) w=weatherScale(m)
    vec4 styleCloud3; // x=densityMultiplier y=lightAbsorption z=ambientStrength w=curlStrength
    vec4 styleCloud4; // x=phaseG y=silverIntensity z=silverSpread w=powderStrength
    vec4 styleCloud5; // x=maxMarchDist(m) y=maxSteps z=lightTaps w=cloudTypeBias
    vec4 styleCloud6; // x=detailStrength (y/z/w reserved)
} uFrame;
