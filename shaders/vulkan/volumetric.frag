#version 460
#extension GL_EXT_ray_query : require
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// Ray-traced volumetric light scattering ("god rays"), half resolution.
// For every pixel: march from the camera toward the depth buffer's surface
// (or volumetricParams.z into the sky), and at each step fire a HARDWARE
// RAY-QUERY shadow ray toward the sun/moon against the scene TLAS. Steps
// that can see the light accumulate in-scatter with a Henyey-Greenstein
// forward lobe; steps in shadow don't -- which is exactly what carves
// crepuscular rays through tree canopies at a low sun.
//
// The scattering medium has its own base density so clear-weather scenes can
// still show shafts; it shares the surface fog's altitude reference and can
// share its height falloff. Output: rgb = in-scattered radiance
// (transmittance-weighted, ready for additive composite), a = the linear
// view distance this pixel marched against (consumed by the composite
// pass's depth-aware upsample).
layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outScatter;

layout(set = 0, binding = 0) uniform sampler2D uDepth;

#include "frameData.glsl"
#include "skyModel.glsl"

layout(set = 2, binding = 0) uniform accelerationStructureEXT uTLAS;
#include "surfaceShadow.glsl"

layout(push_constant) uniform Push {
    mat4 invViewProj; // NDC + depth -> world
} pc;

float volHash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float volHash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Trilinear value noise -- used to perturb the scattering medium's density
// with a slowly-drifting field (see main()'s turbulence block) so light
// shafts shimmer/waver like real dust- or mist-borne rays instead of
// reading as a static, geometrically-fixed cone.
float volNoise3D(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = volHash13(i + vec3(0.0, 0.0, 0.0));
    float n100 = volHash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = volHash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = volHash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = volHash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = volHash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = volHash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = volHash13(i + vec3(1.0, 1.0, 1.0));
    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

// Henyey-Greenstein forward lobe. As g approaches 1 this is only bounded by
// the 1e-6 floor on denom -- looking almost exactly down-sun (mu near 1)
// with a strongly forward-scattering g (the default, 0.87, is well into
// that regime) spikes it by 1-2 orders of magnitude over its typical value
// at even a ~10 degree offset, which used to flood the ENTIRE frame white
// (not just a thin visible ray) once multiplied through by intensity and
// sun radiance. kPhaseCeiling keeps the forward-scatter core dramatic
// (still ~10-30x the ambient-angle phase value) without it running away to
// an unbounded wash the moment the camera happens to look sunward.
const float kPhaseCeiling = 4.0;

float phaseHG(float mu, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * mu;
    return min((1.0 - g2) / (12.566371 * max(denom * sqrt(denom), 1e-6)),
              kPhaseCeiling);
}

// Use the same opacity micromaps as surface and water shadows. Treating a
// needle card as opaque blocked shafts through the gaps visible in its leaves.

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    float depth = texture(uDepth, uv).r;

    // Reconstruct the world position / marchable distance for this pixel.
    vec4 farW = pc.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 worldFar = farW.xyz / farW.w;
    vec3 camPos = uFrame.camPosWS.xyz;
    vec3 rayDir = normalize(worldFar - camPos);

    float surfaceDist = 1e9;
    if (depth < 1.0) {
        vec4 posW = pc.invViewProj * vec4(vNdc, depth, 1.0);
        surfaceDist = distance(posW.xyz / posW.w, camPos);
    }

    float maxDist = max(uFrame.volumetricParams.z, 1.0);
    float marchEnd = min(surfaceDist, maxDist);
    int steps = clamp(int(uFrame.volumetricParams.w + 0.5), 4, 32);
    float g = clamp(uFrame.volumetricParams.y, 0.0, 0.95);
    float intensity = uFrame.volumetricParams.x;

    vec3 L = normalize(-uFrame.lightDir.xyz);
    float mu = dot(rayDir, L);
    float phase = phaseHG(mu, g);

    // Scattering needs its own extinction coefficient: multiplying by the
    // surface fog density made clear presets (and GLGEN_SMOKE_NOFOG) erase
    // god rays even though their toggle and density control were on. Keep the
    // altitude profile anchored to the surface fog reference so shafts still
    // hug the same ground plane, but let the volumetric density control work
    // independently. 0.001 per density unit preserves the old optical depth
    // around the common 0.004 surface-fog / 1.75 volumetric-density setting.
    float densityScale = 0.001 * uFrame.volumetricParams2.x;
    float falloff = max(uFrame.fogParams.w, 0.0) *
                    max(uFrame.volumetricParams2.y, 0.0);
    // Organic shaft movement (editor "God Ray Turbulence"/"Wind Speed"):
    // a coarse, slowly-drifting noise field perturbs density per march
    // step below. Frequency is low (large, tens-of-meters features) so it
    // reads as the whole shaft waving/thickening over time, not per-pixel
    // sparkle; the fixed diagonal wind direction is deliberately not
    // axis-aligned so the drift doesn't look like a horizontal/vertical
    // scroll.
    float turbStrength = uFrame.volumetricParams2.z;
    float windSpeed = uFrame.volumetricParams2.w;
    const vec3 kWindDir = vec3(0.6, 0.2, 0.78);
    vec3 windOffset = kWindDir * uFrame.miscParams.z * windSpeed;

    float dt = marchEnd / float(steps);
    // Dithered start hides banding at low step counts; the composite pass's
    // depth-aware upsample plus fog's low frequency hide the residual noise.
    float t = dt * volHash12(gl_FragCoord.xy);

    vec3 scatter = vec3(0.0);
    float transmittance = 1.0;
    for (int i = 0; i < steps; ++i) {
        vec3 P = camPos + rayDir * t;
        // Surface fog's XZ patchiness is deliberately not sampled here: it is
        // a separate medium, and this loop already evaluates its own 3D
        // turbulence up to 32 times per half-res pixel.
        float density = densityScale *
                        exp(-max(P.y - uFrame.miscParams.y, 0.0) * falloff);
        if (turbStrength > 0.0005) {
            float n = volNoise3D(P * 0.045 + windOffset);
            density *= max(1.0 + (n - 0.5) * 2.0 * turbStrength, 0.0);
        }

        float lit = traceShadow(P, L);
        float segT = exp(-density * dt);
        // Energy-conserving segment: in-scatter integrated across the step.
        scatter += transmittance * lit * (1.0 - segT) * vec3(1.0);
        transmittance *= segT;
        t += dt;
        if (transmittance < 0.005)
            break;
    }

    // No 4*pi normalization: the phase function stays sr^-1 so off-axis
    // views get a SUBTLE haze while the forward lobe toward the sun is
    // ~10x stronger -- that contrast IS the god-ray look. (A normalized
    // version washed the whole frame milky at playable fog densities.)
    // Tint blends OVER the physical sun/moon-radiance color rather than
    // replacing it outright: mix(1,tint,0) is a no-op multiply, so
    // tintStrength=0 (many scenes) reproduces the untinted result exactly.
    vec3 tint = mix(vec3(1.0), uFrame.volumetricTint.rgb, uFrame.volumetricTint.w);
    vec3 radiance = scatter * phase * uFrame.sunRadiance.rgb * intensity * tint;
    // Final safety ceiling on the ADDITIVE contribution this pass hands to
    // the composite. Even with phaseHG's own cap above, a long, fully
    // UNOCCLUDED march (the open-sky case -- nothing for traceShadow to
    // ever fail against, so `scatter` climbs toward its density-limited
    // ceiling for the ENTIRE visible sky near the sun, not just a thin
    // shaft) combined with intensity/sunRadiance still produced values tens
    // of times brighter than the sun disc itself once a few frames of
    // "make god rays more dramatic" tuning had raised march distance,
    // density and intensity together -- painting most of the sun-facing
    // frame solid white instead of a shaft. Capped well below the sun
    // disc's own radiance (~sunOuterRadiance=20 x transmittance) so the
    // scattered glow reads as bright atmosphere around the sun, never
    // brighter than the sun itself.
    const float kMaxShaftRadiance = 1.2;
    radiance = min(radiance, vec3(kMaxShaftRadiance));
    outScatter = vec4(radiance, marchEnd);
}
