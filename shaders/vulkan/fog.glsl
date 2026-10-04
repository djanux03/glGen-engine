#ifndef FOG_GLSL
#define FOG_GLSL

// fog.glsl -- the engine's fog. One implementation, included by every shader
// that needs it (mesh/meshInstanced/terrainChunk/sky/volumetric), replacing
// the three near-identical copies those shaders each used to carry.
//
// WHAT CHANGED, AND WHY IT HAD TO
//
// The old fog was `color = mix(color, skySample, amount)` with
//     amount = clamp((1-exp(-(dist-start)*density)) * exp(-max(y-ref,0)*falloff),
//                    0, maxOpacity)
// Four things were wrong with that, and together they are why distant terrain
// never looked like it belonged to the sky behind it:
//
//  1. `mix` toward a color is not what a participating medium does. A medium
//     ATTENUATES what is behind it (transmittance) and ADDS its own scattered
//     light (in-scatter). Those are separate quantities and only collapse into
//     one lerp when the added light exactly equals the color being lerped
//     toward. Because they were collapsed, fog here could not brighten toward
//     the sun or darken away from it: a lerp has no term that can.
//
//  2. The height term used the SHADED PIXEL's altitude, not the altitude
//     profile along the view ray. Fog is the integral of density over the
//     path; evaluating density at the endpoint answers a different question.
//     Standing in a valley looking up at a peak and standing on that peak
//     looking back down are the same segment and must give the same fog --
//     the old term gave wildly different amounts, neither of them right.
//     Height fog is now the closed-form integral of exp(-h/H) along the ray
//     (Wenzel's form), exact for an exponential profile, one extra exp().
//
//  3. `maxOpacity` hard-clamped the blend, so past some distance every surface
//     froze at 90% fogged and stopped converging -- while the sky behind it
//     was not fogged at all. That discontinuity IS the horizon seam this
//     engine has always had: 90%-fogged terrain meeting 0%-fogged sky along a
//     line. maxOpacity is now a FLOOR on transmittance rather than a ceiling
//     on the blend: same dial, same range, same "keep some surface visible"
//     intent, but at its default of 1 fog runs to completion and terrain
//     dissolves into a horizon that matches, because the sky is handed the
//     same medium (fogSky(), called from sky.frag).
//
//  4. It was disconnected from the atmosphere. sky.frag has had a real
//     single-scattering Rayleigh+Mie model (skyModel.glsl) the whole time
//     while fog was a hand-tuned exponential lerping toward a blurred cubemap
//     tap. Two different models cannot agree at the horizon however the
//     constants are tuned.
//
// THE MODEL
//
// Two media share one path:
//
//   aerial -- air. Extinction uses skyModel.glsl's own Rayleigh + ozone + Mie
//     coefficients, so it is SPECTRAL: blue is removed from a distant surface
//     ~3x faster than red. That spectral difference is what aerial
//     perspective actually is, and it is the part no amount of tinting
//     reproduces. The in-scatter is the sky's own radiance in the view
//     direction (the env cubemap, which sky.frag rendered this frame), which
//     makes the horizon match exact rather than approximate: with
//     source = sigma_scatter * skyAhead, the slab integral S/sigma*(1-T)
//     converges on skyAhead itself as the path grows. A surface at the far
//     plane and the sky one pixel past it end up the same color by
//     construction, not by tuning.
//
//   ground -- mist that pools in low ground. Exponential in altitude with
//     scale height H = 1/fogHeightFalloff, modulated by a slow noise field so
//     it has body instead of reading as an analytic wash. It scatters the sky
//     from ABOVE (skyAbove) plus a Henyey-Greenstein lobe toward the sun,
//     which is what makes low fog glow when you look into a low sun.
//
// Both combine as one homogeneous medium -- optical depths add, in-scatter
// sources add -- so there is a single exp() and no compositing order to get
// wrong:
//     T = exp(-sigma * len),  L = S/sigma * (1 - T),  out = color*T + L
// The ground layer's altitude variation survives that because its analytic
// integral is converted back into a path-averaged coefficient (tau/len),
// which is by definition the constant with the same total optical depth.
//
// Requires frameData.glsl and skyModel.glsl to have been included first (the
// same convention biomeLighting.glsl and paintMaterial.glsl already use --
// the caller owns its includes so this file never double-defines anything).

// Slow, coarse density variation for the ground layer. Deliberately cheap and
// low-frequency: this should read as "the mist is thicker over there", not as
// noise. Two octaves is enough to break the analytic look; a third only
// showed up as shimmer under camera motion.
float fogNoiseHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float fogNoise2D(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = fogNoiseHash(i), b = fogNoiseHash(i + vec2(1, 0));
    float c = fogNoiseHash(i + vec2(0, 1)), d = fogNoiseHash(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Density multiplier in [1-strength, 1+strength] for the ground layer at a
// world XZ, drifting with time. Sampled once per fragment at the segment
// midpoint rather than integrated -- the field's features are tens of meters
// across, far larger than the error that approximation introduces.
float fogGroundNoise(vec2 worldXZ) {
    float strength = uFrame.fogParams2.z;
    if (strength <= 0.001)
        return 1.0;
    vec2 drift = vec2(0.7, 0.31) * uFrame.miscParams.z * uFrame.fogParams3.y;
    vec2 p = worldXZ * max(uFrame.fogParams2.w, 1e-4) + drift;
    float n = fogNoise2D(p) * 0.65 + fogNoise2D(p * 2.7 + 11.3) * 0.35;
    return max(1.0 + (n - 0.5) * 2.0 * strength, 0.0);
}

// Ground-layer density at a world altitude, without the noise term. This is
// the medium volumetric.frag marches through, so god rays thicken and thin
// with exactly the fog the surfaces show instead of with a private copy of
// the falloff curve that could (and did) drift away from it.
// falloffScale lets the volumetric pass make shafts hug the ground more or
// less than the surface fog without owning a second copy of the curve.
float fogGroundDensityScaled(float worldY, float falloffScale) {
    float falloff = max(uFrame.fogParams.w, 0.0) * max(falloffScale, 0.0);
    float h = worldY - uFrame.miscParams.y;
    // falloff == 0 means a uniform slab, not "no fog": the exponential's limit
    // as H -> infinity is a constant, and the 0 end of the editor's Height
    // Falloff slider has always meant "same density at every altitude".
    return uFrame.fogParams.x * exp(-max(h, 0.0) * falloff);
}

float fogGroundDensityAt(float worldY) {
    return fogGroundDensityScaled(worldY, 1.0);
}

// Closed-form optical depth of the exponential-height ground layer along a
// segment: integral over [0,len] of density0 * exp(-(t*dy)/H) dt, with dy the
// ray's vertical component per unit length and H = 1/falloff.
//
// The small-k branch is not an optimization, it is required: the general form
// divides by k = dy*falloff, and a level ray -- looking down a valley, the
// single most common case for ground fog to matter -- has dy exactly 0. The
// limit there is the flat-slab answer, density0 * len.
float fogGroundOpticalDepth(float y0, float dy, float len) {
    float falloff = max(uFrame.fogParams.w, 0.0);
    float density=max(uFrame.fogParams.x,0.0);
    float h0=y0-uFrame.miscParams.y,h1=h0+dy*len;
    // Density is CLAMPED below the reference height in fogGroundDensityAt.
    // Integrating an unclamped exponential instead made descending rays
    // explode and gave opposite views of the same segment different fog.
    if(falloff<1e-5)return density*len;
    if(abs(dy)*len<1e-4)return density*len*exp(-max(h0,0.0)*falloff);
    float low=min(h0,h1),high=max(h0,h1);
    float below=max(min(high,0.0)-low,0.0);
    float span=max(high-max(low,0.0),0.0);
    float x=falloff*span;
    // Evaluate from the lower endpoint so no positive exponent can overflow.
    // Series avoids cancellation for shallow rays over long distances.
    float integral=x<.001?span*(1.0-x*.5+x*x/6.0):(1.0-exp(-x))/falloff;
    return density*(below+exp(-max(low,0.0)*falloff)*integral)/abs(dy);
}

struct FogSample {
    vec3 transmittance; // what survives of the surface behind the fog
    vec3 inscatter;     // what the fog itself adds
    float opacity;      // 1 - luminance(T); debug views only
};

// The whole fog model for one view segment.
//   worldPos    -- the shaded point
//   densityMult -- ground-layer biome density hook (biomeLighting.glsl); 1.0
//                  if unused
//   aerialMult  -- aerial-layer multiplier, for terrain's mountain haze; 1.0
//                  if unused
//   tint        -- biome tint hook; vec3(1) if unused
//   skyAhead    -- env cubemap in the TRUE view direction: what the aerial
//                  layer converges to, and therefore what kills the seam
//   skyAbove    -- env cubemap biased upward: the skylight illuminating the
//                  ground layer, which is lit from above even when the camera
//                  is looking down into it
//
// Distance is derived here from worldPos rather than taken as an argument.
// Every caller used to pass the interpolated view-space Z, which is the
// distance to the camera PLANE, not to the camera: at this engine's FOV a
// pixel in the screen corner is ~15% further away than one in the center at
// the same view Z, and got ~15% too little fog for it. Deriving it in one
// place also removes the possibility of the three scene shaders disagreeing.
FogSample fogAlongSegment(vec3 camPos, vec3 worldPos, float startDistance,
                       float densityMult, float aerialMult,
                       vec3 tint, vec3 skyAhead, vec3 skyAbove) {
    FogSample fog;
    fog.transmittance = vec3(1.0);
    fog.inscatter = vec3(0.0);
    fog.opacity = 0.0;
    if(uFrame.atmosphereParams.x<.5) return fog;

    float dist = distance(worldPos, camPos);
    // Fog begins at fogStart: shrink the segment rather than subtracting from
    // the distance, so the height integral runs over the part of the ray that
    // is actually fogged instead of over a shifted phantom segment.
    float start = clamp(startDistance, 0.0, dist);
    float len = dist - start;
    if (len <= 0.0 || uFrame.fogParams.z <= 0.001)
        return fog;

    vec3 viewDir = normalize(worldPos - camPos);
    vec3 segStart = camPos + viewDir * start;
    vec3 segMid = camPos + viewDir * (start + len * 0.5);

    vec3 L = normalize(-uFrame.lightDir.xyz);
    float mu = dot(viewDir, L);

    // ---- aerial layer: skyModel.glsl's air over this short segment --------
    // Density is evaluated once at the segment midpoint. Over a few hundred
    // meters against 8.5 km / 1.2 km scale heights the profile is flat to well
    // under a percent, so constant-density is not an approximation worth
    // improving.
    float betaM = atmBetaMie(uFrame.styleSkyZenith.w);
    float hMid = max(segMid.y + 4.0, 0.0);
    float densR = exp(-hMid / kAtmHr);
    float densM = exp(-hMid / kAtmHm);
    float aerial = max(uFrame.fogParams2.x, 0.0) * max(aerialMult, 0.0);

    vec3 sigmaScatterAerial =
        (kAtmBetaR * densR + vec3(betaM * densM)) * aerial;
    // Extinction carries ozone and Mie absorption too; scattering does not.
    vec3 sigmaAerial =
        ((kAtmBetaR + kAtmBetaO) * densR + vec3(betaM * kAtmMieAbsorb * densM)) *
        aerial;
    // Source = scattering coefficient * the sky it is scattering. This is the
    // line that makes the horizon match: S/sigma -> skyAhead as the path
    // lengthens, so a far surface and the sky just past it converge on the
    // same value without either side being tuned to the other.
    vec3 srcAerial = sigmaScatterAerial * skyAhead;
    // Explicit forward Mie lobe on top. The cubemap already carries the sun's
    // glow, but only at the resolution of a 128^2 face -- this restores the
    // tight haze flare you get looking into a low sun, and gives the editor a
    // dial for it that does not also brighten the sky away from the sun.
    srcAerial += vec3(betaM * densM * aerial) * uFrame.sunRadiance.rgb *
                 atmMiePhase(mu, 0.76) * uFrame.fogParams2.y;

    // ---- ground layer: exponential mist -----------------------------------
    float tauGround = fogGroundOpticalDepth(segStart.y, viewDir.y, len) *
                      densityMult * fogGroundNoise(segMid.xz);
    // Back to a path average so both layers can share one exponential.
    float sigmaGround = tauGround / max(len, 1e-4);

    // Mist is grey water droplets: it scatters whatever reaches it, which is
    // the sky above plus a strong forward lobe toward the sun. fogDayColor /
    // fogNightColor tint that -- those two editor colors were uploaded every
    // frame and read by no shader at all before this.
    float dayness = smoothstep(-0.12, 0.18, uFrame.sunRadiance.w);
    vec3 groundTint =
        mix(uFrame.fogNightColor.rgb, uFrame.fogDayColor.rgb, dayness) * tint;
    vec3 srcGround =
        sigmaGround * groundTint *
        (skyAbove + uFrame.sunRadiance.rgb *
                        atmPhaseHG(mu, clamp(uFrame.fogParams3.x, 0.0, 0.95)) *
                        uFrame.fogParams2.y);

    // Blizzard whiteout & horizontal blowing snow streaks (The Long Dark)
    if (uFrame.blizzardParams.x > 0.001
#ifdef FOG_AERIAL_ONLY
        && false
#endif
    ) {
        float blizzard = clamp(uFrame.blizzardParams.x, 0.0, 1.0);
        float bSpeed = uFrame.blizzardParams.y;
        vec3 windOffset = vec3(uFrame.miscParams.z * bSpeed, uFrame.miscParams.z * (-bSpeed * 0.2), uFrame.miscParams.z * (bSpeed * 0.4));
        vec3 pB = (segMid + windOffset) * 0.08;
        float snowStreak = fogNoise2D(vec2(pB.x * 0.35 + pB.z * 0.15, pB.y * 2.8 + pB.x * 0.7));
        float blizzardDensity = blizzard * (0.012 + snowStreak * 0.022);
        sigmaGround += blizzardDensity;
        vec3 snowScatter = mix(vec3(0.92, 0.95, 0.98), skyAbove, 0.25);
        srcGround += blizzardDensity * snowScatter;
    }

    // ---- one medium -------------------------------------------------------
    vec3 sigma = sigmaAerial + vec3(sigmaGround);
    vec3 src = srcAerial + srcGround;
    vec3 T = exp(-sigma * len);
    // S/sigma*(1-T) is the analytic in-scatter of a homogeneous slab. The
    // guard matters: sigma is exactly 0 whenever fog is dialled off, and the
    // limit there is src*len, not a division by zero.
    vec3 Lscat = mix(src * len, src / max(sigma, vec3(1e-8)) * (1.0 - T),
                     step(vec3(1e-8), sigma));

    // maxOpacity as a transmittance FLOOR (see this file's header): 1 lets fog
    // run to completion, 0 disables it outright, and values between mean what
    // they always did -- never lose more than this much of the surface.
    vec3 floorT = vec3(1.0 - clamp(uFrame.fogParams.z, 0.0, 1.0));
#ifdef FOG_AERIAL_ONLY
    floorT=vec3(0); // the compositor clamps the combined transport once
#endif
    fog.transmittance = max(T, floorT);
    // Scale the added light by the same clamp, or a scene with fog dialled
    // down still receives full in-scatter on top of an unattenuated surface
    // and ends up brighter than the same scene with fog on.
    vec3 admitted =
        min((1.0 - fog.transmittance) / max(1.0 - T, vec3(1e-5)), vec3(1.0));
    fog.inscatter = max(Lscat * admitted, vec3(0.0));
    fog.opacity = 1.0 - dot(fog.transmittance, vec3(0.2126, 0.7152, 0.0722));
    return fog;
}

FogSample fogAlongView(vec3 worldPos, float densityMult, float aerialMult,
                       vec3 tint, vec3 skyAhead, vec3 skyAbove) {
    return fogAlongSegment(uFrame.camPosWS.xyz,worldPos,uFrame.fogParams.y,
                           densityMult,aerialMult,tint,skyAhead,skyAbove);
}

vec3 fogApply(vec3 color, FogSample fog) {
    return color * fog.transmittance + fog.inscatter;
}

// The sky's share of the ground layer. Without this the seam only moves:
// surfaces would dissolve correctly and then stop against a sky with no fog
// in it. A sky ray is infinitely long, so the ground layer's optical depth is
// its integral to infinity -- finite for any ray that climbs (density0/k),
// unbounded for one that descends, which is why the descending case is capped
// rather than evaluated.
//
// The aerial layer is deliberately NOT applied here: its in-scatter converges
// on the sky's own radiance, so running it over an infinite sky path would be
// asking the sky to converge on itself.
//
// Only the VISIBLE sky gets this. The environment cubemap must not -- surfaces
// sample it back as the color their own fog scatters, and fogging it there
// would apply the medium twice.
vec3 fogSky(vec3 color, vec3 viewDir, vec3 skyAbove) {
    float strength = clamp(uFrame.fogParams3.z, 0.0, 1.0);
    if (strength <= 0.001 || uFrame.fogParams.z <= 0.001)
        return color;
    float falloff = max(uFrame.fogParams.w, 0.0);
    float density0 = fogGroundDensityAt(uFrame.camPosWS.xyz.y);
    float k = viewDir.y * falloff;
    // 4000 m stands in for "infinite" on the level/descending branches: far
    // enough past the far plane to saturate, near enough to keep exp() in a
    // range where the result is a clean 0 rather than a denormal.
    float tau = k > 1e-5 ? density0/k :
        fogGroundOpticalDepth(uFrame.camPosWS.y,viewDir.y,4000.0);
    if(k>1e-5&&uFrame.camPosWS.y<uFrame.miscParams.y)
        tau+=uFrame.fogParams.x*(uFrame.miscParams.y-uFrame.camPosWS.y)/viewDir.y;
    tau *= strength;

    vec3 L = normalize(-uFrame.lightDir.xyz);
    float mu = dot(viewDir, L);
    float dayness = smoothstep(-0.12, 0.18, uFrame.sunRadiance.w);
    vec3 groundTint =
        mix(uFrame.fogNightColor.rgb, uFrame.fogDayColor.rgb, dayness);
    vec3 src = groundTint *
               (skyAbove + uFrame.sunRadiance.rgb *
                               atmPhaseHG(mu, clamp(uFrame.fogParams3.x, 0.0, 0.95)) *
                               uFrame.fogParams2.y);

    float T = max(exp(-tau), 1.0 - clamp(uFrame.fogParams.z, 0.0, 1.0));
    return color * T + src * (1.0 - T);
}

#endif
