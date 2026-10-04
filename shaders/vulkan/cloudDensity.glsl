// Shared visible-cloud and cloud-shadow density; world metres and identical weather.
#ifndef CLOUD_NOISE_SET
#define CLOUD_NOISE_SET 0
#endif
layout(set=CLOUD_NOISE_SET,binding=0) uniform sampler3D uShapeNoise;
layout(set=CLOUD_NOISE_SET,binding=1) uniform sampler3D uDetailNoise;
layout(set=CLOUD_NOISE_SET,binding=2) uniform sampler2D uWeather;
layout(set=CLOUD_NOISE_SET,binding=3) uniform sampler2D uCurl;
layout(set=CLOUD_NOISE_SET,binding=4) uniform sampler3D uPeriodicDetailNoise;
#ifndef CLOUD_SAMPLE_2D
#ifdef CLOUD_COMPUTE_DENSITY
#define CLOUD_SAMPLE_2D(image,uv) textureLod(image,uv,0)
#else
#define CLOUD_SAMPLE_2D(image,uv) texture(image,uv)
#endif
#endif
#ifndef CLOUD_FOOTPRINT
#define CLOUD_FOOTPRINT 0.0
#endif
#ifndef CLOUD_SAMPLE_2D_LOD
#define CLOUD_SAMPLE_2D_LOD(image,uv,lod) CLOUD_SAMPLE_2D(image,uv)
#endif
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
    float weatherLod=log2(max(CLOUD_FOOTPRINT*float(textureSize(uWeather,0).x)/scale,1.0));
    vec3 w0 = CLOUD_SAMPLE_2D_LOD(uWeather, uv,weatherLod).rgb;
    // 0.31 is deliberately not a round fraction of 1: a rational ratio would
    // put both octaves' seams on the same lattice.
    vec2 uv2 = (mat2(0.80, -0.60, 0.60, 0.80) * (wpos.xz + clWindOffset() * 0.6)) /
               (scale * 0.31);
    vec3 w1 = CLOUD_SAMPLE_2D_LOD(uWeather, uv2,max(weatherLod-log2(.31),0.0)).rgb;

    float coverage = mix(w0.r, w0.r * w1.r * 1.6, 0.55);
    float cov = clamp(uFrame.styleCloud0.x, 0.0, 1.0);
    // The weather map modulates authored coverage; it must not leave cloud
    // islands behind when the author selects a completely clear sky.
    if(cov<=0.0) return vec3(0,w0.g,clamp(w0.b+uFrame.styleCloud5.w,0,1));
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
    if(weather.x<=0.0)return 0.0;
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
    // Coverage places clouds; it must not also weaken every occupied voxel.
    // Multiplying it a second time left low-coverage skies with only tiny
    // residual caps after detail erosion, rather than sparse solid cumulus.
    return uFrame.atmosphereParams.x>.5 ? base : base * coverage;
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
        float curlLod=log2(max(CLOUD_FOOTPRINT*float(textureSize(uCurl,0).x)/(detailScale*4.0),1.0));
        vec3 curl = CLOUD_SAMPLE_2D_LOD(uCurl, wpos.xz / (detailScale * 4.0),curlLod).rgb * 2.0 - 1.0;
        p += curl * curlStrength * detailScale * 0.5 * (1.0 - clamp(h * 3.0, 0.0, 1.0));
    }

    vec4 n = uFrame.atmosphereParams.x>.5?textureLod(uPeriodicDetailNoise,p/detailScale,mip):
                                          textureLod(uDetailNoise, p / detailScale, mip);

    if(uFrame.atmosphereParams.x>.5) {
        // The authored volume stores inverted F1 (bright cellular interiors),
        // not Nubis' precomposed curl/alligator channels. Erode with distance
        // from those interiors so the new input carves connected billows.
        // Interpreting all four channels as the legacy pack erased their
        // interiors and retained disconnected, poorly shaded fragments.
        float erosion=1.0-dot(n.gba,vec3(.625,.25,.125));
        erosion*=clamp(uFrame.styleCloud6.x,0.0,1.0);
        return clErosion(profile,erosion);
    }

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
