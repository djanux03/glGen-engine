// skyModel.glsl -- physically-based atmosphere core, shared by sky.frag (the
// visible sky + the per-frame environment-cubemap faces) and volumetric.frag
// (fog in-scatter tinting). Replaces the old 3-constant gradient sky.
//
// Model: single-scattering Rayleigh + Mie around a spherical planet with
// exponential density profiles, ozone absorption folded into the Rayleigh
// profile, and closed-form sun/moon transmittance via Schuler's Chapman
// grazing-incidence approximation (GPU Pro 3) -- no nested raymarch, so the
// in-scatter integral is O(steps), cheap enough for a full-res pass.
//
// Everything is parameterized (no uFrame / push-constant access) so callers
// with different binding layouts can all include it. Distances are METERS.
// Directions: "sunDir"/"moonDir" here always point TOWARD the light (the
// engine's FrameData lightDir is the direction light TRAVELS -- negate it
// before calling).
//
// The C++ mirror of atmSunTransmittance() lives in VulkanRenderer.cpp
// (atmSunTransmittanceCpu) -- it packs FrameDataGpu.sunRadiance so lit
// shaders never evaluate the atmosphere themselves. Keep the two in sync.

const float ATM_PI = 3.14159265359;

// Planet + profile constants (Earth-ish; Hillaire/Bruneton-style betas).
const float kAtmRg = 6371e3;  // ground radius
const float kAtmRa = 6451e3;  // top-of-atmosphere radius (80 km shell)
const float kAtmHr = 8500.0;  // Rayleigh scale height
const float kAtmHm = 1200.0;  // Mie scale height
const vec3 kAtmBetaR = vec3(5.802e-6, 13.558e-6, 33.1e-6); // Rayleigh scatter
// Ozone absorption approximated on the Rayleigh profile (real ozone sits in
// a ~25 km tent; using the Rayleigh column keeps Chapman closed-form and
// preserves the visual effect that matters: twilight zeniths stay blue
// instead of drifting green-yellow).
const vec3 kAtmBetaO = vec3(1.15e-6, 3.32e-6, 0.16e-6);
const float kAtmMieAbsorb = 1.11; // Mie extinction = scatter * 1.11

// Haze dial (0..1) -> Mie scattering coefficient. 0 = crystalline alpine
// air, 1 = heavy humid haze.
float atmBetaMie(float haze) {
    return mix(2.0e-6, 2.4e-5, haze * haze);
}

float atmRayleighPhase(float mu) {
    return 3.0 / (16.0 * ATM_PI) * (1.0 + mu * mu);
}

// Cornette-Shanks Mie phase.
float atmMiePhase(float mu, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * mu;
    return 3.0 * (1.0 - g2) * (1.0 + mu * mu) /
           (8.0 * ATM_PI * (2.0 + g2) * max(denom * sqrt(denom), 1e-6));
}

// Henyey-Greenstein phase (volumetric fog uses this cheaper lobe).
float atmPhaseHG(float mu, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * mu;
    return (1.0 - g2) / (4.0 * ATM_PI * max(denom * sqrt(denom), 1e-6));
}

// Schuler's Chapman grazing-incidence approximation: relative airmass along
// a ray leaving radius x (in scale heights) at zenith cosine cosChi, for an
// exponential atmosphere. Optical depth = beta * H * exp(-h) * Ch.
float atmChapman(float x, float cosChi) {
    float c = sqrt(1.57079632679 * x);
    if (cosChi >= 0.0)
        return c / ((c - 1.0) * cosChi + 1.0);
    // Below-horizontal: dip to the grazing radius, then back out. exp guard
    // keeps float finite; the clamped-huge airmass still drives
    // transmittance to a clean 0.
    float sinChi = sqrt(clamp(1.0 - cosChi * cosChi, 0.0, 1.0));
    float x0 = x * sinChi;
    float c0 = sqrt(1.57079632679 * x0);
    return 2.0 * c0 * exp(min(x - x0, 80.0)) -
           c / ((c - 1.0) * (-cosChi) + 1.0);
}

// Transmittance from a point P (planet-centered coords) to space toward a
// light. This is THE sun-color function: at noon it is near-white, at the
// horizon it strips blue then green, leaving the sun deep orange-red, and a
// few degrees below the horizon it is ~0.
vec3 atmTransmittanceToSpace(vec3 P, vec3 toLight, float betaMie) {
    float r = length(P);
    float cosChi = dot(P / r, toLight);
    float hR = max(r - kAtmRg, 0.0) / kAtmHr;
    float hM = max(r - kAtmRg, 0.0) / kAtmHm;
    float amR = kAtmHr * exp(-hR) * atmChapman(r / kAtmHr, cosChi);
    float amM = kAtmHm * exp(-hM) * atmChapman(r / kAtmHm, cosChi);
    vec3 tau = (kAtmBetaR + kAtmBetaO) * amR +
               vec3(betaMie * kAtmMieAbsorb) * amM;
    return exp(-tau);
}

// Ray/sphere for a sphere of radius R centered at origin. Returns
// (tNear, tFar); no intersection -> tFar < 0.
vec2 atmRaySphere(vec3 ro, vec3 rd, float R) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - R * R;
    float disc = b * b - c;
    if (disc < 0.0)
        return vec2(1e9, -1.0);
    float s = sqrt(disc);
    return vec2(-b - s, -b + s);
}

// Camera world-height (engine meters, sea level ~-2) -> planet-centered
// position for the atmosphere. Clamped so diving below terrain or flying a
// debug camera to silly heights never breaks the model.
vec3 atmPlanetPos(float worldY) {
    return vec3(0.0, kAtmRg + clamp(worldY + 4.0, 2.0, 40000.0), 0.0);
}

struct AtmSample {
    vec3 radiance;      // in-scattered light along the ray
    vec3 transmittance; // view-path transmittance (1 = clear)
    float groundT;      // ray distance to the virtual ground, <0 if none
};

// Single-scatter integral along (ro, rd) for two light sources (sun + moon;
// pass moonOuter = vec3(0) to skip). `outerSun`/`outerMoon` are the lights'
// radiance at the top of the atmosphere. The virtual ground contributes a
// simple albedo bounce so environment-map ambient from below the horizon is
// grass-green-gray, not void black.
AtmSample atmScatter(vec3 ro, vec3 rd, vec3 toSun, vec3 outerSun,
                     vec3 toMoon, vec3 outerMoon, float haze, int steps) {
    AtmSample res;
    res.radiance = vec3(0.0);
    res.transmittance = vec3(1.0);
    res.groundT = -1.0;

    float betaM = atmBetaMie(haze);

    vec2 atmHit = atmRaySphere(ro, rd, kAtmRa);
    float tEnd = max(atmHit.y, 0.0);
    vec2 gndHit = atmRaySphere(ro, rd, kAtmRg);
    bool hitsGround = gndHit.y > 0.0 && gndHit.x > 0.0;
    if (hitsGround) {
        tEnd = min(tEnd, gndHit.x);
        res.groundT = gndHit.x;
    }
    if (tEnd <= 0.0)
        return res;
    // A near-horizon ray's chord through the 80 km shell is ~2000 km; with
    // a handful of samples the dense first kilometers get catastrophically
    // undersampled (each segment averages air across ~100+ km of falloff),
    // which smeared a blown-white halo across half the sky at low sun.
    // Everything past a couple hundred km is extinct anyway -- clamp, and
    // distribute samples quadratically so most land in the near, dense air.
    tEnd = min(tEnd, 250e3);

    bool hasMoon = dot(outerMoon, outerMoon) > 1e-12;
    float muS = dot(rd, toSun);
    float muM = dot(rd, toMoon);
    float phRS = atmRayleighPhase(muS);
    float phMS = atmMiePhase(muS, 0.76);
    float phRM = atmRayleighPhase(muM);
    float phMM = atmMiePhase(muM, 0.76);

    float tPrev = 0.0;
    for (int i = 0; i < steps; ++i) {
        float u = (float(i) + 1.0) / float(steps);
        float tNext = u * u * tEnd; // quadratic: dense near the camera
        float dt = tNext - tPrev;
        vec3 P = ro + rd * (0.5 * (tPrev + tNext));
        tPrev = tNext;
        float h = max(length(P) - kAtmRg, 0.0);
        float dR = exp(-h / kAtmHr);
        float dM = exp(-h / kAtmHm);

        vec3 sigmaT = (kAtmBetaR + kAtmBetaO) * dR +
                      vec3(betaM * kAtmMieAbsorb) * dM;
        vec3 stepT = exp(-sigmaT * dt);

        vec3 inscatter =
            atmTransmittanceToSpace(P, toSun, betaM) * outerSun *
            (kAtmBetaR * dR * phRS + vec3(betaM * dM * phMS));
        if (hasMoon)
            inscatter += atmTransmittanceToSpace(P, toMoon, betaM) * outerMoon *
                         (kAtmBetaR * dR * phRM + vec3(betaM * dM * phMM));

        // Energy-conserving segment integration (Frostbite-style).
        res.radiance +=
            res.transmittance * (inscatter - inscatter * stepT) / max(sigmaT, vec3(1e-9));
        res.transmittance *= stepT;
    }

    // Virtual ground bounce (matters for the env cubemap's lower hemisphere:
    // grass-field bounce light, not black). Dark meadow albedo -- bright
    // enough to bounce believable green-gray fill light onto undersides,
    // dark enough that the below-horizon band doesn't glare through fog.
    if (hitsGround) {
        vec3 Pg = ro + rd * res.groundT;
        vec3 upG = normalize(Pg);
        const vec3 kGroundAlbedo = vec3(0.062, 0.072, 0.048);
        vec3 direct =
            outerSun * atmTransmittanceToSpace(Pg, toSun, betaM) *
                max(dot(upG, toSun), 0.0) +
            (hasMoon ? outerMoon * atmTransmittanceToSpace(Pg, toMoon, betaM) *
                           max(dot(upG, toMoon), 0.0)
                     : vec3(0.0));
        vec3 ground = kGroundAlbedo / ATM_PI * direct + kGroundAlbedo * res.radiance;
        res.radiance += res.transmittance * ground;
    }
    return res;
}
