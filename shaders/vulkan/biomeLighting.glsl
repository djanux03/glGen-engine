// biomeLighting.glsl -- R3 of MEADOW_TERRAIN_REVAMP_PLAN.md §5.5: per-pixel
// biome lighting & atmosphere. Every function here takes the caller's
// already-computed (wMeadow, wForest, wMountain) weights and blends
// CONTINUOUSLY -- never a hard switch, same rule as the material splat and
// the landform field itself. Included by terrainChunk.frag; a future prop/
// vegetation shader could reuse it once R4 bakes per-instance biome weights
// (see the plan's R3 phase note on why props are out of scope this pass).
//
// Depends on frameData.glsl having been included first (reads uFrame.biome*
// fields) -- no #include here to avoid double-inclusion; the caller is
// expected to #include "frameData.glsl" before this file, exactly like
// every other shared include in this shader set.
//
// Self-contained hashing (bl_-prefixed) rather than reusing the caller's
// own hash helpers, since this file may be included by shaders whose hash
// function names/signatures differ (mesh.frag, terrainChunk.frag, and
// grassMaterial.glsl's gm-prefixed ones are all slightly different).
float bl_hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// ---------------------------------------------------------------------------
// (a) Surface response: ambient tint/intensity, blended by biome weight.
// ---------------------------------------------------------------------------

// Returns (tint.rgb, intensityScale) already blended by the 3 weights and
// faded toward neutral (1,1,1,1) by the global strength dial
// (uFrame.biomeDirectParams.w) -- multiply your sky-irradiance ambient by
// tint*intensityScale.
vec4 biomeAmbient(float wMeadow, float wForest, float wMountain) {
    vec3 tint = uFrame.biomeAmbientMeadow.rgb * wMeadow +
               uFrame.biomeAmbientForest.rgb * wForest +
               uFrame.biomeAmbientMountain.rgb * wMountain;
    float intensity = uFrame.biomeAmbientMeadow.w * wMeadow +
                      uFrame.biomeAmbientForest.w * wForest +
                      uFrame.biomeAmbientMountain.w * wMountain;
    float strength = uFrame.biomeDirectParams.w;
    return vec4(mix(vec3(1.0), tint, strength), mix(1.0, intensity, strength));
}

// Forest canopy occlusion + a large, slowly-drifting dappled light-shaft
// mask (sells "light through leaves" without real canopy shadow maps).
// `sunDirXZ` is the horizontal component of the light direction (used to
// slant the mask so shafts appear to lean away from the sun, like real
// canopy dapple), `time` is uFrame.miscParams.z (already used for grass
// dew/wind -- same clock, no new uniform).
float forestCanopyDirect(vec3 worldPos, float wForest, float time, vec2 sunDirXZ) {
    float strength = uFrame.biomeDirectParams.w * wForest;
    if (strength <= 0.001)
        return 1.0;
    float occlusion = mix(1.0, uFrame.biomeDirectParams.x, strength);

    // Coarse, soft dapple: a couple of octaves of cheap sine-hash noise,
    // slanted by the sun and drifting slowly with time (real canopy sways).
    vec2 p = worldPos.xz * 0.07 + sunDirXZ * (worldPos.y * 0.12) +
            vec2(time * 0.025, time * 0.017);
    vec2 cell = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = bl_hash12(cell), b = bl_hash12(cell + vec2(1, 0));
    float c = bl_hash12(cell + vec2(0, 1)), d = bl_hash12(cell + vec2(1, 1));
    float dapple = mix(mix(a, b, u.x), mix(c, d, u.x), u.y);

    float shaft = mix(1.0, 0.55 + 0.45 * dapple,
                      uFrame.biomeDirectParams.y * strength);
    return occlusion * shaft;
}

// Direct-sun multiplier for thin, crisp mountain air -- slightly brighter/
// crisper than lowland, per the plan's "1.05: thin clear air".
float mountainDirectMultiplier(float wMountain) {
    float strength = uFrame.biomeDirectParams.w * wMountain;
    return mix(1.0, uFrame.biomeDirectParams.z, strength);
}

// ---------------------------------------------------------------------------
// (b) Atmosphere response: biome-weighted fog density/tint + mountain
// aerial perspective.
// ---------------------------------------------------------------------------

// Multiplies the base fog density term (before the exp() falloff) --
// forest denser (wood air), mountains clearer near the camera.
float biomeFogDensityMult(float wForest, float wMountain) {
    float strength = uFrame.biomeDirectParams.w;
    float mult = 1.0;
    mult = mix(mult, mult * uFrame.biomeFogForest.w, wForest * strength);
    mult = mix(mult, mult * uFrame.biomeFogMountain.w, wMountain * strength);
    return mult;
}

// Tints the sky-sampled fog color toward each biome's atmosphere color.
vec3 biomeFogTint(vec3 baseFogColor, float wForest, float wMountain) {
    float strength = uFrame.biomeDirectParams.w;
    vec3 tint = mix(vec3(1.0), uFrame.biomeFogForest.rgb, wForest * strength);
    tint = mix(tint, tint * uFrame.biomeFogMountain.rgb, wMountain * strength);
    return baseFogColor * tint;
}

// Extra aerial-perspective term for mountains: distant, lower ridgelines
// fade into blue haze faster than the base curve alone would give (plan:
// "stronger blue aerial-perspective term scaling with distance AND the
// ridge's altitude drop"). `altitudeDrop` is how far below the camera this
// pixel sits (clamped positive; distant LOWER ridges read hazier, distant
// peaks at camera height don't).
//
// Returns a MULTIPLIER on the aerial layer's density (>= 1), not an opacity
// to add. Under the old fog it added straight onto the blend factor, which
// meant it pushed pixels toward the flat fog color rather than through more
// air -- so "blue aerial perspective" came out whatever color the fog tint
// happened to be. fog.glsl's aerial extinction is spectral, so thickening it
// produces the blue shift on its own, from the Rayleigh coefficients, and
// this term no longer has to fake the color it is named after.
//
// The distance term is gone with it: optical depth already scales with path
// length by construction. Doubling down on distance here is what made far
// ridges saturate to a flat wall.
float mountainAerialPerspective(float altitudeDrop, float wMountain) {
    float strength = uFrame.biomeDirectParams.w * wMountain * uFrame.biomeMountainExtra.x;
    if (strength <= 0.001)
        return 1.0;
    float altTerm = clamp(altitudeDrop * 0.01, 0.0, 1.0);
    return 1.0 + (0.6 + 1.9 * altTerm) * strength;
}
