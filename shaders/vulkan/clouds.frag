#version 450
#extension GL_GOOGLE_include_directive : require

// ---------------------------------------------------------------------------
// Volumetric cloudscape -- half-resolution raymarch.
// ---------------------------------------------------------------------------
// Replaces the analytic five-octave fbm deck that used to live in sky.frag.
// The density model is Nubis: a low-frequency Perlin-Worley "dimensional
// profile" carved by a height gradient and a weather map, then eroded by the
// Nubis3 four-channel detail noise (R/G curl-alligator -> wispy, B/A
// alligator -> billowy). That erosion is what produces cauliflower cumulus
// silhouettes instead of the soft blobs an fbm threshold gives you, and it is
// the single biggest reason this looks different from what it replaced.
//
// Lighting is Nubis2/3's light-energy model -- transmittance toward the sun,
// an in-scatter probability driven by depth-in-cloud and height, and a dual
// Henyey-Greenstein lobe whose second term is the silver lining -- but it is
// driven by uFrame.sunRadiance (already atmosphere-coloured on the CPU)
// rather than the reference's own Preetham sky, so clouds go white at noon
// and gold at dusk with no special case, and the moon takes over at night for
// free (sunRadiance IS the active light after the twilight handoff).
//
// Output is premultiplied: rgb = light scattered toward the eye, a =
// transmittance. cloudComposite.frag upsamples and blends it over the sky
// with (ONE, SRC_ALPHA).
//
// Coordinate note: the layer is a pair of spheres concentric with the
// atmosphere planet (skyModel.glsl), so distant clouds compress into a band
// and end at a real horizon; but noise is sampled at TRUE world XZ
// (camPosWS + rd*t), not at the planet-local position, so the field is
// continuous across the world and does not swim as the camera moves.

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outCloud;

#define FRAME_DATA_SET 1
#include "frameData.glsl"
#include "skyModel.glsl"

layout(set = 0, binding = 0) uniform sampler3D uShapeNoise;  // Perlin-Worley base
layout(set = 0, binding = 1) uniform sampler3D uDetailNoise; // Nubis 4-channel detail
layout(set = 0, binding = 2) uniform sampler2D uWeather;     // r=coverage g=wetness b=type
layout(set = 0, binding = 3) uniform sampler2D uCurl;        // curl noise, for wispy bases

layout(push_constant) uniform Push {
    mat4 invViewProj;
    vec4 jitter; // x = temporal dither phase, yzw unused
} pc;

// --- dials ---------------------------------------------------------------
// styleCloud0 x=coverage y=softness zw=wind
// styleCloud1 x=layerBottom(m) y=featureScale(m, env deck only) z=opticalDensity w=sunOcclusion
// styleCloud2 x=layerThickness(m) y=shapeScale(m) z=detailScale(m) w=weatherScale(m)
// styleCloud3 x=densityMultiplier y=lightAbsorption z=ambientStrength w=curlStrength
// styleCloud4 x=phaseG y=silverIntensity z=silverSpread w=powderStrength
// styleCloud5 x=maxMarchDist(m) y=maxSteps z=lightTaps w=cloudTypeBias
// styleCloud6 x=detailStrength

// --- toolbox (Nubis naming kept so the reference reads across) ------------
float clRemap(float v, float oldMin, float oldMax, float newMin, float newMax) {
    return newMin + ((v - oldMin) / (oldMax - oldMin)) * (newMax - newMin);
}

float clRemapClamped(float v, float oldMin, float oldMax, float newMin, float newMax) {
    float t = clamp((v - oldMin) / max(oldMax - oldMin, 1e-6), 0.0, 1.0);
    return newMin + t * (newMax - newMin);
}

// Derived from Set-Range: uses oldMin to erode (positive) or inflate the
// input. This IS the detail-erosion operator -- subtracting noise from a
// smooth profile and renormalising is what carves billows out of a blob.
float clErosion(float v, float oldMin) {
    return clamp((v - oldMin) / max(1.0 - oldMin, 1e-6), 0.0, 1.0);
}

float clHash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float clHG(float mu, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * mu;
    return (1.0 - g2) / (12.566371 * max(denom * sqrt(denom), 1e-4));
}

// --- layer geometry ------------------------------------------------------
float clLayerBottom() { return max(uFrame.styleCloud1.x, 50.0); }
float clLayerThickness() { return max(uFrame.styleCloud2.x, 50.0); }
float clLayerTop() { return clLayerBottom() + clLayerThickness(); }

// Height gradients per cloud type. type 0 = stratus (flat sheet low in the
// layer), 0.5 = stratocumulus, 1 = cumulus (tall, rounded, reaching most of
// the way up). Mixing the three by type is what lets one weather map drive a
// sky that has both flat overcast patches and towering heaps.
float clHeightGradient(float h, float type) {
    h = clamp(h, 0.0, 1.0);
    float stratus = clamp(clRemap(h, 0.0, 0.07, 0.0, 1.0), 0.0, 1.0) *
                    clamp(clRemap(h, 0.20, 0.32, 1.0, 0.0), 0.0, 1.0);
    float stratocumulus = clamp(clRemap(h, 0.02, 0.20, 0.0, 1.0), 0.0, 1.0) *
                          clamp(clRemap(h, 0.45, 0.68, 1.0, 0.0), 0.0, 1.0);
    float cumulus = clamp(clRemap(h, 0.01, 0.12, 0.0, 1.0), 0.0, 1.0) *
                    clamp(clRemap(h, 0.65, 0.98, 1.0, 0.0), 0.0, 1.0);
    // At type 0.5 both branches already evaluate to stratocumulus, so this
    // single mix is exact at all three anchors despite looking like it
    // double-counts.
    float d1 = mix(stratus, stratocumulus, clamp(type * 2.0, 0.0, 1.0));
    float d2 = mix(stratocumulus, cumulus, clamp((type - 0.5) * 2.0, 0.0, 1.0));
    return mix(d1, d2, clamp(type, 0.0, 1.0));
}

vec2 clWindOffset() {
    return uFrame.styleCloud0.zw * uFrame.miscParams.z * 40.0;
}

// Weather: coverage / type. The 512x512 map is tiled at weatherScale metres,
// which repeats visibly over a large view, so a second sample at an
// incommensurate scale and a rotated frame breaks the grid up. The coverage
// dial then biases the result, so the slider means roughly what it says
// (0 = clear, 1 = overcast) rather than "add this to whatever the texture had".
vec3 clSampleWeather(vec3 wpos) {
    float scale = max(uFrame.styleCloud2.w, 100.0);
    vec2 uv = (wpos.xz + clWindOffset()) / scale;
    vec3 w0 = texture(uWeather, uv).rgb;
    // 0.31 is deliberately not a round fraction of 1: a rational ratio would
    // put both octaves' seams on the same lattice.
    vec2 uv2 = (mat2(0.80, -0.60, 0.60, 0.80) * (wpos.xz + clWindOffset() * 0.6)) /
               (scale * 0.31);
    vec3 w1 = texture(uWeather, uv2).rgb;

    float coverage = mix(w0.r, w0.r * w1.r * 1.6, 0.55);
    float cov = clamp(uFrame.styleCloud0.x, 0.0, 1.0);
    // Raw map coverage is bunched around its mean; stretch about 0.5 first so
    // the dial has usable travel at both ends instead of saturating early.
    coverage = clamp((coverage - 0.5) * 1.9 + 0.5, 0.0, 1.0);
    coverage = clamp(clRemap(cov, 0.0, 1.0, coverage - 0.55, coverage + 0.55), 0.0, 1.0);

    float type = clamp(w0.b + uFrame.styleCloud5.w, 0.0, 1.0);
    return vec3(coverage, w0.g, type);
}

// --- density -------------------------------------------------------------
// The smooth "dimensional profile": everything the detail pass erodes. In
// Nubis3 this comes out of a VDB; here it is built from the Perlin-Worley
// volume x height gradient x weather, which is what gives a whole SKY of
// clouds rather than one modelled cloud in a box.
float clProfileDensity(vec3 wpos, float h, vec3 weather, float mip) {
    float shapeScale = max(uFrame.styleCloud2.y, 50.0);
    vec3 p = wpos;
    p.xz += clWindOffset();
    // Skew with height: the top of a cloud lags downwind of its base, which
    // is most of what stops a deck reading as an extruded 2D pattern.
    p.xz += h * 400.0 * normalize(uFrame.styleCloud0.zw + vec2(1e-4));

    vec4 lowFreq = textureLod(uShapeNoise, p / shapeScale, mip);
    // R is Perlin-Worley; GBA are Worley octaves, combined into an fbm that
    // erodes the base into connected billows.
    float worleyFbm = lowFreq.g * 0.625 + lowFreq.b * 0.25 + lowFreq.a * 0.125;
    float base = clRemapClamped(lowFreq.r, worleyFbm - 1.0, 1.0, 0.0, 1.0);

    base *= clHeightGradient(h, weather.z);

    // Anvil: high in the layer, coverage widens so tall clouds spread out at
    // their tops instead of ending in a flat lid.
    float coverage = pow(weather.x, clRemapClamped(h, 0.7, 0.9, 1.0, 0.72));
    float soft = max(uFrame.styleCloud0.y, 0.01);
    base = clRemapClamped(base, clamp(1.0 - coverage - soft, 0.0, 1.0), 1.0, 0.0, 1.0);
    return base * coverage;
}

// Nubis3's GetUprezzedVoxelCloudDensity: the detail erosion that makes the
// silhouette. wispy comes from the curl-alligator channels and dominates thin
// edges; billowy comes from the alligator channels and dominates dense cores.
float clDetailDensity(vec3 wpos, float h, float profile, float type, float dist,
                      float mip) {
    if (profile <= 0.0)
        return 0.0;

    float detailScale = max(uFrame.styleCloud2.z, 5.0);
    vec3 p = wpos;
    p.xz += clWindOffset() * 2.2; // detail drifts faster than the base shape
    p.y -= uFrame.miscParams.z * 6.0; // slow upward boil

    // Curl distorts the bases into wisps -- the reference applies it only
    // where the cloud is thin, which is where real clouds shear.
    float curlStrength = uFrame.styleCloud3.w;
    if (curlStrength > 0.001) {
        vec3 curl = texture(uCurl, wpos.xz / (detailScale * 4.0)).rgb * 2.0 - 1.0;
        p += curl * curlStrength * detailScale * 0.5 * (1.0 - clamp(h * 3.0, 0.0, 1.0));
    }

    vec4 n = textureLod(uDetailNoise, p / detailScale, mip);

    float wispy = mix(n.r, n.g, profile);
    float billowyGradient = pow(max(profile, 1e-4), 0.25);
    float billowy = mix(n.b * 0.3, n.a * 0.3, billowyGradient);
    float composite = mix(wispy, billowy, type);

    // Highest-frequency detail, near the camera only. The triangle-wave
    // folds (abs(abs(x*2-1)*2-1)) turn a smooth channel into sharp ridges,
    // which is what reads as fine cauliflower at close range; blended out
    // with distance because at range it is pure aliasing.
    float hfBlend = clRemapClamped(dist, 1500.0, 6000.0, 0.0, 1.0);
    if (hfBlend < 0.999) {
        float hhfWisps = 1.0 - pow(abs(abs(n.g * 2.0 - 1.0) * 2.0 - 1.0), 4.0);
        float hhfBillows = pow(abs(abs(n.a * 2.0 - 1.0) * 2.0 - 1.0), 2.0);
        float hhf = clamp(mix(hhfWisps, hhfBillows, type), 0.0, 1.0);
        composite = mix(hhf, composite, mix(0.9, 1.0, hfBlend));
    }

    composite *= clamp(uFrame.styleCloud6.x, 0.0, 1.0);

    float d = clErosion(profile, composite);
    // Sharpening. Without this the eroded field is mushy: a low exponent on
    // thin regions pushes them toward solid, which is what gives the crisp
    // lit edge against the sky.
    d = pow(clamp(d, 0.0, 1.0), mix(0.35, 0.62, clamp(profile, 0.0, 1.0)));
    // Fade the finest structure out with distance so half-res sampling of a
    // 128^3 tile does not turn into shimmer at the horizon.
    d *= clRemapClamped(dist, 200.0, 3000.0, 0.55, 1.0);
    return clamp(d, 0.0, 1.0);
}

// --- sun transmittance ---------------------------------------------------
// Cone-tap march toward the light. Six taps spread in a widening cone (the
// spread is what softens self-shadowing into something that reads as
// multiple scattering rather than a hard shadow), plus one long tap that
// catches a distant bank blocking the sun. Profile density only -- running
// the detail erosion inside the light loop costs 6x the texture fetches for
// a difference that the exp() flattens anyway.
//
// The result is DIMENSIONLESS: a weighted mean of the profile density along
// the cone, in 0..1, which the caller turns into an optical depth by scaling
// with cloudLightAbsorption. Integrating real metres here instead is what a
// first pass did, and it does not work: the cone is roughly a layer thickness
// long, so a half-dense path accumulated ~1500 density-metres, every exp()
// downstream underflowed to zero, and the clouds rendered as black cutouts
// against the sky. Keeping the light term unit-free means one dial sets how
// deep self-shadowing goes regardless of how thick the layer is authored.
const vec3 kConeOffsets[6] = vec3[6](
    vec3( 0.20,  0.10,  0.30), vec3(-0.25,  0.15, -0.10),
    vec3( 0.10, -0.20, -0.30), vec3(-0.15, -0.10,  0.25),
    vec3( 0.30,  0.25, -0.20), vec3( 0.00,  0.00,  0.00));

float clDensityToLight(vec3 wpos, vec3 toLight, int taps) {
    // Cone length is tied to the layer, not to the view step: how far light
    // has to travel through cloud to reach this point is a property of the
    // cloud, and has nothing to do with how finely this ray happens to march.
    float coneLen = clLayerThickness() * 0.55;
    float total = 0.0;
    float weight = 0.0;
    for (int i = 0; i < 6; ++i) {
        if (i >= taps)
            break;
        // Taps bunch toward the sample point, where occlusion matters most.
        float u = (float(i) + 0.5) / float(taps);
        float t = coneLen * u * u;
        vec3 p = wpos + toLight * t + kConeOffsets[i] * t;
        float h = (p.y - clLayerBottom()) / clLayerThickness();
        float w = 1.0 - 0.5 * u;
        if (h >= 0.0 && h <= 1.0) {
            // Mip climbs with cone distance: far taps are blurry by
            // construction, which is both cheaper and closer to the truth.
            total += clProfileDensity(p, h, clSampleWeather(p), float(i) * 0.5) * w;
        }
        weight += w;
    }
    // One long tap for a distant bank standing between this point and the
    // sun -- the thing that puts a whole cloud into another's shadow.
    vec3 far = wpos + toLight * coneLen * 4.0;
    float hf = (far.y - clLayerBottom()) / clLayerThickness();
    if (hf >= 0.0 && hf <= 1.0)
        total += clProfileDensity(far, hf, clSampleWeather(far), 3.0) * 0.6;
    weight += 0.6;

    return total / max(weight, 1e-4);
}

// --- layer intersection --------------------------------------------------
// Returns (tEnter, tExit) against the two concentric shells, handling the
// camera being below, inside, or above the layer. tExit < tEnter means miss.
vec2 clLayerInterval(vec3 ro, vec3 rd, float camHeight) {
    float rInner = kAtmRg + clLayerBottom();
    float rOuter = kAtmRg + clLayerTop();
    vec2 inner = atmRaySphere(ro, rd, rInner);
    vec2 outer = atmRaySphere(ro, rd, rOuter);

    if (camHeight < clLayerBottom()) {
        // Below: enter at the inner shell's far root, leave at the outer's.
        if (outer.y <= 0.0)
            return vec2(1.0, -1.0);
        return vec2(max(inner.y, 0.0), max(outer.y, 0.0));
    }
    if (camHeight > clLayerTop()) {
        // Above, looking down: enter at the outer shell's near root and stop
        // at the inner shell if the ray reaches it, otherwise at the outer's
        // far root (a grazing ray that passes through and out again).
        if (outer.y <= 0.0)
            return vec2(1.0, -1.0);
        float enter = max(outer.x, 0.0);
        float exitT = (inner.y > 0.0 && inner.x > 0.0) ? inner.x : max(outer.y, 0.0);
        return vec2(enter, exitT);
    }
    // Inside the layer.
    float exitT = max(outer.y, 0.0);
    if (inner.y > 0.0 && inner.x > 0.0)
        exitT = min(exitT, inner.x);
    return vec2(0.0, exitT);
}

void main() {
    vec4 farW = pc.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 camPos = uFrame.camPosWS.xyz;
    vec3 rd = normalize(farW.xyz / farW.w - camPos);

    // Planet-local origin drives the layer intersection (so the deck curves
    // to a horizon); world position drives the noise (so the field is stable
    // and continuous as the camera translates).
    vec3 ro = atmPlanetPos(camPos.y);
    float camHeight = camPos.y;

    vec2 interval = clLayerInterval(ro, rd, camHeight);
    float tEnter = interval.x;
    float tExit = interval.y;

    outCloud = vec4(0.0, 0.0, 0.0, 1.0);
    if (tExit <= tEnter)
        return;

    // A ray a few degrees above the horizon crosses an effectively unbounded
    // slab; without this cap the step size explodes and the deck dissolves
    // into stripes exactly where it should be densest.
    float maxDist = max(uFrame.styleCloud5.x, 1000.0);
    tExit = min(tExit, tEnter + maxDist);

    int maxSteps = clamp(int(uFrame.styleCloud5.y + 0.5), 24, 192);
    int lightTaps = clamp(int(uFrame.styleCloud5.z + 0.5), 1, 6);

    float span = tExit - tEnter;
    // Step size is tied to the LAYER, not to the span. Dividing the span by
    // the step budget (the obvious first move) means a near-horizon ray --
    // which crosses tens of kilometres of layer -- gets half-kilometre steps
    // and dissolves into horizontal stripes exactly where the deck should be
    // densest and most detailed. Instead: a fine step sized to resolve the
    // layer, grown linearly with distance so the far half of the ray is cheap.
    // That growth IS Nubis3's adaptive step size, and it is why the budget
    // reaches the horizon at all.
    float fineStep = max(clLayerThickness() / 48.0, span / float(maxSteps * 4));
    const float kCoarseMul = 3.0;
    // Doubles the step roughly every 10 km, so the near field stays crisp and
    // a 40 km ray still finishes inside the loop bound.
    const float kStepGrowth = 1.0 / 10000.0;
    // Air extinction between eye and cloud. 1/16 km washes out distant horizon
    // clouds (15-30 km) into the pale horizon sky color while keeping near clouds sharp.
    const float kAerialRate = 1.0 / 16000.0;

    vec3 toLight = normalize(-uFrame.lightDir.xyz);
    float mu = dot(rd, toLight);

    float g = clamp(uFrame.styleCloud4.x, 0.0, 0.95);
    float silverIntensity = uFrame.styleCloud4.y;
    float silverSpread = uFrame.styleCloud4.z;
    // Dual lobe: a broad forward lobe for the general brightening toward the
    // sun, plus a tight one that only fires within a few degrees of it --
    // that second term is the silver lining on a backlit cloud edge.
    float phase = max(clHG(mu, g),
                      silverIntensity * clHG(mu, clamp(0.99 - silverSpread, -0.95, 0.95)));

    float densityMul = max(uFrame.styleCloud3.x, 0.0);
    float lightAbsorb = max(uFrame.styleCloud3.y, 0.0);
    float powderStrength = clamp(uFrame.styleCloud4.w, 0.0, 1.0);

    // Ambient: the sky's own radiance straight up, which is the dominant term
    // lighting a cloud's flanks and underside. Six steps is plenty for a
    // single scalar, and it means the clouds inherit the engine's atmosphere
    // (haze dial, twilight, moonlight) rather than carrying a second sky model.
    AtmSample ambientSample =
        atmScatter(ro, vec3(0.0, 1.0, 0.0), toLight, vec3(uFrame.lightDir.w),
                   toLight, vec3(0.0), uFrame.styleSkyZenith.w, 6);
    vec3 skyAmbient = ambientSample.radiance * uFrame.styleSkyHorizon.w *
                      max(uFrame.styleCloud3.z, 0.0);
    // Undersides see the ground, not the zenith -- darker and warmer.
    vec3 groundAmbient = skyAmbient * vec3(0.42, 0.40, 0.36) * 0.35;

    vec3 sunColor = uFrame.sunRadiance.rgb;

    // Dither the entry point so the step lattice does not band, using a sub-step
    // jitter amplitude to avoid high-frequency pixel stippling on cloud edges.
    float dither = (clHash12(gl_FragCoord.xy + pc.jitter.x) - 0.5) * 0.35;

    vec3 scattered = vec3(0.0);
    float transmittance = 1.0;

    float t = tEnter + fineStep * dither;
    bool refining = false;
    int misses = 0;
    float tFirstHit = tExit;
    bool hasHit = false;

    for (int i = 0; i < maxSteps * 3 && t < tExit; ++i) {
        // Adaptive: the same fine/coarse pair, both scaled up with distance.
        float grow = 1.0 + t * kStepGrowth;
        float baseStep = fineStep * grow;
        float coarseStep = baseStep * kCoarseMul;
        float step = refining ? baseStep : coarseStep;

        vec3 wpos = camPos + rd * t;
        // Height comes from the SPHERE, not from wpos.y: a cloud 40 km away
        // must sit lower in the layer than one overhead, or the deck reads as
        // an infinite flat ceiling.
        float planetH = length(ro + rd * t) - kAtmRg;
        float h = (planetH - clLayerBottom()) / clLayerThickness();
        if (h < 0.0 || h > 1.0) {
            t += step;
            continue;
        }

        vec3 weather = clSampleWeather(wpos);
        float mip = clRemapClamped(t, 2000.0, 40000.0, 0.0, 3.0);
        float profile = clProfileDensity(wpos, h, weather, mip);

        if (profile <= 0.0) {
            if (refining && ++misses >= 6)
                refining = false;
            t += step;
            continue;
        }

        if (!refining) {
            // First hit on a coarse step: back up and re-enter at the fine
            // rate, so the cloud's leading edge is not chopped off at 3x the
            // step. Costs one wasted iteration per cloud entered.
            refining = true;
            misses = 0;
            t = max(t - coarseStep, tEnter);
            continue;
        }
        misses = 0;

        float density = clDetailDensity(wpos, h, profile, weather.z, t, mip);
        if (density <= 0.0005) {
            t += step;
            continue;
        }

        if (!hasHit) {
            tFirstHit = t;
            hasHit = true;
        }

        float sigmaT = density * densityMul;
        float segT = exp(-sigmaT * step);

        // --- light energy (Nubis) ---------------------------------------
        float densityToLight = clDensityToLight(wpos, toLight, lightTaps) *
                               lightAbsorb * uFrame.styleCloud1.w;

        // Attenuation: Beer's law, but floored by a second, shallower curve.
        // Pure Beer goes black in cloud cores; real cores are lit by multiply
        // scattered light, and this two-term form is the cheap stand-in the
        // Nubis slides use.
        float primary = exp(-densityToLight);
        float secondary = exp(-densityToLight * 0.25) * 0.7;
        float attenuation = max(primary,
                                clRemapClamped(mu, 0.7, 1.0, secondary, secondary * 0.25));

        // In-scatter probability: deeper in the cloud and higher in the layer
        // means more chances for a photon to have scattered toward the eye.
        float depthProb = mix(0.05 + pow(clamp(profile, 0.0, 1.0),
                                         clRemapClamped(h, 0.3, 0.85, 0.5, 2.0)),
                              1.0, clamp(densityToLight * 2.0, 0.0, 1.0));
        float verticalProb = pow(clRemapClamped(h, 0.07, 0.30, 0.12, 1.0), 0.8);
        float inScatter = clamp(depthProb * verticalProb, 0.0, 1.0);

        // Powder: thin edges scatter less back at you than their optical
        // depth alone predicts, which darkens the rims of clouds seen against
        // the sun. Only applied on the sun-facing side, hence the mu ramp.
        float powder = 1.0 - exp(-sigmaT * 6.0);
        powder = mix(1.0, powder, powderStrength * clamp(-mu * 0.5 + 0.5, 0.0, 1.0));

        vec3 sunLight = sunColor * attenuation * inScatter * phase * powder;

        // Multiple scattering, Wrenninge-style: two extra octaves with
        // progressively weaker extinction and flatter phase. Costs no new
        // samples and is most of what keeps dense cores from going flat grey.
        float msAtten = exp(-densityToLight * 0.35) * 0.45;
        float msPhase = mix(phase, 0.25, 0.5);
        sunLight += sunColor * msAtten * msPhase * 0.5;
        float msAtten2 = exp(-densityToLight * 0.12) * 0.18;
        sunLight += sunColor * msAtten2 * 0.15;

        vec3 ambient = mix(groundAmbient, skyAmbient, clamp(h * 1.3, 0.0, 1.0));

        // Authored tints, applied to a physical result rather than replacing
        // it -- same division of labour the old analytic deck settled on.
        vec3 tint = mix(uFrame.styleCloudBase.rgb, uFrame.styleCloudMid.rgb,
                        smoothstep(0.15, 0.75, profile));
        tint = mix(tint, uFrame.styleCloudLit.rgb, clamp(attenuation, 0.0, 1.0) * 0.6);

        vec3 luminance = (sunLight + ambient) * tint;

        // Energy-conserving segment integration (Frostbite): the analytic
        // integral of in-scatter across the step, not a point sample of it.
        vec3 integrated = (luminance * sigmaT - luminance * sigmaT * segT) /
                          max(sigmaT, 1e-6);

        // Aerial perspective. Fifty kilometres of air between the eye and a
        // cloud is not clear: it extinguishes the cloud's light and replaces
        // it with its own glow, which is why a distant bank reads as a pale
        // silhouette rather than a small sharp one. Doing this per step
        // rather than once at the end also fixes what the horizon looked like
        // without it -- the far field is where step size has grown the most,
        // and the raw march there resolves into horizontal stripes; washing
        // it toward the sky colour dissolves them into haze, which is both
        // cheaper and more correct than paying for more steps out there.
        // Aerial perspective extinction along the ray path.
        float aerial = exp(-t * kAerialRate);
        integrated = integrated * aerial;

        scattered += transmittance * integrated;
        transmittance *= segT;

        if (transmittance < 0.005) {
            transmittance = 0.0;
            break;
        }
        t += step;
    }

    // Apply distance atmospheric haze fade: as ray distance to the cloud climbs
    // toward the horizon (15 km - 35 km), atmospheric extinction washes out the
    // cloud's contrast and opacity, dissolving it into the background horizon sky color.
    if (hasHit) {
        float aerialFade = exp(-tFirstHit * kAerialRate);
        float cloudAlpha = (1.0 - clamp(transmittance, 0.0, 1.0)) * aerialFade;
        outCloud = vec4(scattered * aerialFade, clamp(1.0 - cloudAlpha, 0.0, 1.0));
    } else {
        outCloud = vec4(0.0, 0.0, 0.0, 1.0);
    }
}
