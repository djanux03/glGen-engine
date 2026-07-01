#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHDR;
uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform float uExposure;
uniform float uGamma;
uniform float uTime;

// solid sky
uniform bool uUseSolidSky;
uniform vec3 uSkyTop;
uniform vec3 uSkyHorizon;


uniform mat3 uSkyRot; // rotates worldDir before sampling

// Sun uniforms
uniform vec3 uSunDir;
uniform vec3 uSunColor;
uniform float uSunSize; // e.g. 0.9995 for small disc
uniform float uSunDiscIntensity;
uniform float uSunHaloIntensity;
uniform float uSunRaysIntensity;
uniform float uSunDiscSoftness;
uniform float uSunHaloSize;
uniform float uSunRaySharpness;
uniform bool uUseBlackHole;
uniform vec3 uBlackHoleDir;
uniform float uBlackHoleSize;
uniform float uBlackHoleDiskTilt;
uniform float uBlackHoleDiskInclination;
uniform vec3 uBlackHoleColor;
uniform float uBlackHoleRingIntensity;
uniform float uBlackHoleRingWidth;
uniform float uBlackHoleDistortion;
uniform float uBlackHoleHaloIntensity;
uniform float uBlackHoleDiskSpinSpeed;
uniform float uBlackHoleDiskTurbulence;
uniform float uBlackHoleChromaticAberration;
uniform float uBlackHoleEclipseStrength;
uniform float uBlackHolePhotonRingIntensity;
uniform float uBlackHoleDopplerBoost;
uniform float uBlackHoleJetIntensity;
uniform float uBlackHoleCoronaIntensity;
uniform float uBlackHoleStarLensIntensity;
uniform float uBlackHoleShadowStrength;
uniform float uSkyAtmosphereStrength;
uniform float uSkyGradientPower;
uniform float uSkyHorizonGlow;
uniform float uNightFactor;
uniform float uStarIntensity;
uniform float uMilkyWayIntensity;
uniform vec3 uNightHorizonGlow;
uniform float uNightDither;

// Lightweight procedural sky cloud controls
uniform bool uSkyCloudsEnabled;
uniform float uSkyCloudScale;
uniform float uSkyCloudCoverage;
uniform float uSkyCloudDensity;
uniform float uSkyCloudSoftness;
uniform float uSkyCloudSpeed;
uniform vec3 uSkyCloudColor;

const float PI = 3.14159265359;



float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        v += amp * noise(p);
        p = p * 2.03 + vec2(11.7, 3.1);
        amp *= 0.5;
    }
    return v;
}

vec2 dirToEquirectUV(vec3 d)
{
    d = normalize(d);
    float u = atan(d.z, d.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5;
    return vec2(u, v);
}

float starField(vec3 dir)
{
    vec2 uv = dirToEquirectUV(dir);
    vec2 g = uv * 2048.0;
    vec2 cell = floor(g);
    float h = hash21(cell);
    float star = smoothstep(0.9975, 1.0, h);
    float twinkle = 0.6 + 0.4 * sin(uTime * 2.0 + h * 123.4);
    return star * twinkle;
}

// Compute HDR value that produces the desired LDR color after ACES + gamma.
// Input: target color in sRGB (what the user picks in the UI).
// Output: linear HDR value that, after ACES tonemapping + pow(1/2.2),
//         closely reproduces the input sRGB color.
vec3 inverseACES(vec3 srgb)
{
    // sRGB -> linear target (what we want after ACES + gamma)
    vec3 t = pow(max(srgb, vec3(0.0)), vec3(2.2));
    // Solve the ACES curve: t = (x*(2.51*x+0.03)) / (x*(2.43*x+0.59)+0.14)
    // Rearranged: (2.43*t - 2.51)*x^2 + (0.59*t - 0.03)*x + 0.14*t = 0
    // Use quadratic formula on each channel.
    vec3 A = 2.43 * t - 2.51;
    vec3 B = 0.59 * t - 0.03;
    vec3 C = 0.14 * t;
    vec3 disc = max(B * B - 4.0 * A * C, vec3(0.0));
    // A is negative for normal color values (t < ~1.03), so use the
    // (-B - sqrt) root to get a positive result.
    vec3 x = (-B - sqrt(disc)) / (2.0 * A);
    return max(x, vec3(0.0));
}

vec3 calculateAtmosphere(vec3 rayDir, vec3 sunDir);

vec3 customSkyGradient(vec3 worldDir)
{
    float vertical = clamp(worldDir.y * 0.5 + 0.5, 0.0, 1.0);
    vertical = pow(vertical, max(uSkyGradientPower, 0.001));
    vec3 sky = inverseACES(mix(uSkyHorizon, uSkyTop, vertical)) * uExposure;

    float horizon = pow(1.0 - clamp(abs(worldDir.y), 0.0, 1.0), 2.3);
    sky += inverseACES(uSkyHorizon) * (horizon * max(uSkyHorizonGlow, 0.0));
    return sky;
}

vec3 sampleBaseSky(vec3 worldDir, vec3 sunDir, bool solidSky)
{
    if (solidSky) {
        vec3 customSky = customSkyGradient(worldDir);
        float lowSunDamp = mix(0.34, 1.0, smoothstep(0.02, 0.34, sunDir.y));
        float atmosphereBlend =
            clamp(uSkyAtmosphereStrength, 0.0, 1.0) * lowSunDamp;
        if (atmosphereBlend > 0.001) {
            vec3 atmosphereSky = calculateAtmosphere(worldDir, sunDir);
            return mix(customSky, atmosphereSky, atmosphereBlend);
        }
        return customSky;
    }

    vec2 uv = dirToEquirectUV(worldDir);
    vec3 hdr = texture(uHDR, uv).rgb;
    return hdr * uExposure;
}

mat2 rotate2D(float a)
{
    float s = sin(a);
    float c = cos(a);
    return mat2(c, -s, s, c);
}

vec3 blackHoleSpaceBackdrop(vec3 worldDir)
{
    vec2 uv = dirToEquirectUV(worldDir);
    vec3 base = inverseACES(vec3(0.006, 0.008, 0.016));

    float bandAxis =
        dot(worldDir, normalize(vec3(-0.22, 0.62, 0.75)));
    float milkyBand = exp(-bandAxis * bandAxis * 13.0);
    float dust = fbm(uv * 12.0 + vec2(0.0, uTime * 0.0015));
    float darkDust = fbm(uv * 34.0 + vec2(11.0, -7.0));
    vec3 nebula = vec3(0.10, 0.14, 0.28) * milkyBand * dust *
                  (1.0 - darkDust * 0.45);

    float stars = starField(worldDir);
    float tinyStars = smoothstep(0.992, 1.0,
                                 hash21(floor(uv * vec2(2900.0, 1450.0))));
    vec3 starColor = vec3(1.25, 1.18, 1.02) * stars +
                     vec3(0.55, 0.68, 1.0) * tinyStars * 0.30;

    return (base + nebula + starColor) * uExposure;
}

float cloudShape(vec2 uv)
{
    vec2 warp = vec2(fbm(uv * 0.36 + vec2(7.4, uTime * 0.012)),
                     fbm(uv * 0.31 + vec2(-5.1, uTime * 0.009))) - 0.5;
    uv += warp * 2.2;

    float broad = fbm(uv * 0.70);
    float puffs = fbm(uv * 1.75 + vec2(13.7, -2.8));
    float detail = fbm(uv * 5.50 + vec2(-19.0, 8.5));
    float wisps = fbm(uv * 12.0 + vec2(3.2, -14.8));
    return broad * 0.58 + puffs * 0.32 + detail * 0.17 - wisps * 0.10;
}

float cloudCoverageMask(vec2 uv)
{
    float shape = cloudShape(uv);
    float threshold = mix(0.74, 0.30, clamp(uSkyCloudCoverage, 0.0, 1.0));
    float softness = max(uSkyCloudSoftness, 0.015);
    float cloud = smoothstep(threshold, threshold + softness, shape);
    float erosion = smoothstep(0.18, 0.86, fbm(uv * 8.0 + vec2(1.7, 21.0)));
    cloud *= mix(0.55, 1.0, erosion);
    return clamp(cloud, 0.0, 1.0);
}

vec3 applySkyClouds(vec3 mapped, vec3 worldDir, vec3 sunDir)
{
    if (!uSkyCloudsEnabled || uSkyCloudDensity <= 0.001)
        return mapped;

    float horizonFade = smoothstep(0.015, 0.18, worldDir.y);
    float zenithFade = 1.0 - smoothstep(0.82, 1.0, worldDir.y) * 0.35;
    if (horizonFade <= 0.001)
        return mapped;

    float parallax = 1.0 / max(worldDir.y + 0.18, 0.08);
    vec2 uv = worldDir.xz * parallax * max(uSkyCloudScale, 0.001);
    vec2 wind = vec2(0.82, 0.34) * uTime * uSkyCloudSpeed * 18.0;
    uv += wind;

    float cloud = cloudCoverageMask(uv) * horizonFade * zenithFade;
    cloud = clamp(cloud * uSkyCloudDensity, 0.0, 1.0);
    if (cloud <= 0.002)
        return mapped;

    float eps = 0.035;
    float dx = cloudCoverageMask(uv + vec2(eps, 0.0)) -
               cloudCoverageMask(uv - vec2(eps, 0.0));
    float dz = cloudCoverageMask(uv + vec2(0.0, eps)) -
               cloudCoverageMask(uv - vec2(0.0, eps));
    vec3 cloudNormal = normalize(vec3(-dx * 2.4, 0.82, -dz * 2.4));

    float sunFacing = clamp(dot(cloudNormal, sunDir) * 0.5 + 0.5, 0.0, 1.0);
    float forward = pow(max(dot(worldDir, sunDir), 0.0), 8.0);
    float edge = smoothstep(0.05, 0.55, cloud) *
                 (1.0 - smoothstep(0.62, 1.0, cloud));
    float silver = forward * edge * 1.25;

    vec3 cloudBase = inverseACES(clamp(uSkyCloudColor, vec3(0.0), vec3(1.0)));
    vec3 coolShadow = cloudBase * vec3(0.50, 0.56, 0.66);
    vec3 warmLight = cloudBase * (0.72 + 0.48 * sunFacing) +
                     inverseACES(clamp(uSunColor, vec3(0.0), vec3(1.0))) *
                         (0.22 + silver);
    vec3 cloudColor = mix(coolShadow, warmLight, sunFacing);

    float alpha = clamp(cloud * uSkyCloudDensity * 0.78, 0.0, 0.92);
    return mix(mapped, cloudColor * uExposure, alpha);
}


// --- Atmospheric Scattering Parameters ---
const float R_EARTH = 6360000.0;
const float R_ATMOSPHERE = 6420000.0;
const vec3 BETA_RAYLEIGH = vec3(5.8e-6, 13.5e-6, 33.1e-6);
const float BETA_MIE = 21e-6;
const float H_RAYLEIGH = 8000.0;
const float H_MIE = 1200.0;
const float G_MIE = 0.76;

// Intersection with a sphere centered at origin
vec2 sphereIntersect(vec3 rayOrigin, vec3 rayDir, float radius) {
    float b = dot(rayOrigin, rayDir);
    float c = dot(rayOrigin, rayOrigin) - radius * radius;
    float d = b * b - c;
    if (d < 0.0) return vec2(-1.0);
    float sqrtD = sqrt(d);
    return vec2(-b - sqrtD, -b + sqrtD);
}

// Single-scattering atmospheric raymarching
vec3 calculateAtmosphere(vec3 rayDir, vec3 sunDir) {
    vec3 rayOrigin = vec3(0.0, R_EARTH + 1.0, 0.0);
    vec2 atmIntersection = sphereIntersect(rayOrigin, rayDir, R_ATMOSPHERE);
    if (atmIntersection.y < 0.0) return vec3(0.0); // Looking out to space

    float tMin = max(0.0, atmIntersection.x);
    float tMax = atmIntersection.y;
    vec2 earthIntersection = sphereIntersect(rayOrigin, rayDir, R_EARTH);
    if (earthIntersection.x > 0.0) tMax = min(tMax, earthIntersection.x); // Blocked by earth

    int numSamples = 16;
    int numSamplesLight = 8;
    float segmentLength = (tMax - tMin) / float(numSamples);
    float tCurrent = tMin + segmentLength * 0.5;

    vec3 totalRayleigh = vec3(0.0);
    vec3 totalMie = vec3(0.0);
    float opticalDepthR = 0.0;
    float opticalDepthM = 0.0;

    float mu = dot(rayDir, sunDir);
    float phaseR = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
    float phaseM = 3.0 / (8.0 * PI) * ((1.0 - G_MIE * G_MIE) * (1.0 + mu * mu)) / 
                   ((2.0 + G_MIE * G_MIE) * pow(1.0 + G_MIE * G_MIE - 2.0 * G_MIE * mu, 1.5));

    for (int i = 0; i < numSamples; ++i) {
        vec3 samplePos = rayOrigin + rayDir * tCurrent;
        float height = length(samplePos) - R_EARTH;
        
        float hr = exp(-height / H_RAYLEIGH) * segmentLength;
        float hm = exp(-height / H_MIE) * segmentLength;
        opticalDepthR += hr;
        opticalDepthM += hm;

        // Light marching
        vec2 lightAtmIntersect = sphereIntersect(samplePos, sunDir, R_ATMOSPHERE);
        float segmentLengthLight = lightAtmIntersect.y / float(numSamplesLight);
        float tCurrentLight = segmentLengthLight * 0.5;
        float opticalDepthLightR = 0.0;
        float opticalDepthLightM = 0.0;
        
        bool inEarthShadow = false;
        vec2 lightEarthIntersect = sphereIntersect(samplePos, sunDir, R_EARTH);
        if (lightEarthIntersect.x > 0.0) inEarthShadow = true;

        if (!inEarthShadow) {
            for (int j = 0; j < numSamplesLight; ++j) {
                vec3 samplePosLight = samplePos + sunDir * tCurrentLight;
                float heightLight = length(samplePosLight) - R_EARTH;
                opticalDepthLightR += exp(-heightLight / H_RAYLEIGH) * segmentLengthLight;
                opticalDepthLightM += exp(-heightLight / H_MIE) * segmentLengthLight;
                tCurrentLight += segmentLengthLight;
            }

            vec3 tau = BETA_RAYLEIGH * (opticalDepthR + opticalDepthLightR) + 
                       BETA_MIE * 1.1 * (opticalDepthM + opticalDepthLightM);
            vec3 attenuation = exp(-tau);
            
            totalRayleigh += hr * attenuation;
            totalMie += hm * attenuation;
        }
        tCurrent += segmentLength;
    }
    
    vec3 sunIntensity = vec3(22.0); // Base sun intensity
    return (totalRayleigh * BETA_RAYLEIGH * phaseR + totalMie * BETA_MIE * phaseM) * sunIntensity;
}

void main()
{
    // Reconstruct view ray in view space from screen UV
    vec2 ndc = vUV * 2.0 - 1.0;
    vec4 clip = vec4(ndc, 1.0, 1.0);

    vec4 viewPos = uInvProj * clip;
    viewPos /= viewPos.w;

    vec3 viewDir = normalize(viewPos.xyz);

    // Rotate into world (w=0 so translation is ignored)
    vec3 worldDir = normalize((uInvView * vec4(viewDir, 0.0)).xyz);

    // Apply user sky rotation (XYZ) then sample equirect HDR
    worldDir = normalize(uSkyRot * worldDir);
    vec3 mapped;
    vec3 sunDir = normalize(-uSunDir);
    vec3 blackHoleDir = normalize(uBlackHoleDir);

    bool solidSky = uUseSolidSky;
    mapped = sampleBaseSky(worldDir, sunDir, solidSky);
    float eventHorizonMask = 0.0;
    float eventShadowMask = 0.0;
    vec3 foregroundDiskAdd = vec3(0.0);

    if (uUseBlackHole) {
        float eventRadius =
            max(acos(clamp(uBlackHoleSize, -0.999999, 0.999999)), 0.0009);
        // A black hole shadow appears much larger than the true event horizon
        // because the photon sphere captures and redirects nearby light.
        float shadowRadius = eventRadius * 2.05;
        float holeDot = clamp(dot(worldDir, blackHoleDir), -1.0, 1.0);
        float holeAngle = acos(holeDot);
        float shadowRadial = holeAngle / max(shadowRadius, 0.0009);
        float radialAA = max(fwidth(shadowRadial) * 1.5, 0.0015);

        eventHorizonMask =
            1.0 - smoothstep(0.93 - radialAA, 1.00 + radialAA, shadowRadial);
        eventShadowMask =
            1.0 - smoothstep(1.00 + radialAA, 1.48 + radialAA, shadowRadial);
        float outsideShadow =
            smoothstep(0.98 - radialAA, 1.08 + radialAA, shadowRadial);

        vec3 refUp = abs(blackHoleDir.y) > 0.94 ? vec3(1.0, 0.0, 0.0)
                                                : vec3(0.0, 1.0, 0.0);
        vec3 holeRight = normalize(cross(refUp, blackHoleDir));
        vec3 holeUp = normalize(cross(blackHoleDir, holeRight));
        vec3 radialVec = worldDir - blackHoleDir * holeDot;
        vec3 radialDir = length(radialVec) > 0.00001
                             ? normalize(radialVec)
                             : holeRight;

        float shadowScale = max(sin(shadowRadius), 0.0005);
        vec2 holeUv =
            vec2(dot(worldDir, holeRight), dot(worldDir, holeUp)) / shadowScale;
        vec2 diskBasisUv = rotate2D(uBlackHoleDiskTilt) * holeUv;

        vec3 blackHoleBackdrop = blackHoleSpaceBackdrop(worldDir);

        float lensReach = 6.5 + uBlackHoleDistortion * 4.5;
        float lensMask =
            1.0 - smoothstep(1.12, lensReach, shadowRadial);
        float closePass = max(shadowRadial - 0.72, 0.08);
        float bend =
            uBlackHoleDistortion *
            (0.36 / closePass + 0.16 / max(shadowRadial + 0.12, 0.12));
        bend = clamp(bend * lensMask, 0.0, 2.4);

        float sourceTheta =
            clamp(holeAngle + bend * shadowRadius * 4.8, 0.0, PI - 0.001);
        float chromaOffset =
            shadowRadius * bend * clamp(uBlackHoleChromaticAberration, 0.0, 1.0) *
            0.55;
        vec3 lensedDirG =
            normalize(blackHoleDir * cos(sourceTheta) +
                      radialDir * sin(sourceTheta));
        vec3 lensedDirR =
            normalize(blackHoleDir * cos(min(sourceTheta + chromaOffset, PI)) +
                      radialDir * sin(min(sourceTheta + chromaOffset, PI)));
        vec3 lensedDirB =
            normalize(blackHoleDir * cos(max(sourceTheta - chromaOffset, 0.0)) +
                      radialDir * sin(max(sourceTheta - chromaOffset, 0.0)));
        vec3 lensedSkyR = blackHoleSpaceBackdrop(lensedDirR);
        vec3 lensedSkyG = blackHoleSpaceBackdrop(lensedDirG);
        vec3 lensedSkyB = blackHoleSpaceBackdrop(lensedDirB);
        vec3 lensedSky = vec3(lensedSkyR.r, lensedSkyG.g, lensedSkyB.b);
        mapped = mix(blackHoleBackdrop, lensedSky,
                     clamp(lensMask * (0.45 + bend * 0.30), 0.0, 0.90));

        float ringWidth =
            mix(0.045, 0.32, clamp(uBlackHoleRingWidth, 0.02, 0.90));
        float diskInclination =
            clamp(uBlackHoleDiskInclination, 0.0, radians(88.0));
        float inclinationSin = sin(diskInclination);
        float projectedMinor = max(cos(diskInclination), 0.035);
        float diskDepth = diskBasisUv.y / projectedMinor;
        float perspective =
            clamp(1.0 + diskDepth * inclinationSin * 0.105, 0.76, 1.30);
        vec2 diskUv = vec2(diskBasisUv.x / perspective, diskDepth);
        float nearSide = smoothstep(0.42, -0.20, diskBasisUv.y);
        float farSide = smoothstep(-0.08, 0.46, diskBasisUv.y);
        float depthLight = mix(0.70, 1.22, nearSide) * mix(1.0, 0.74, farSide);

        float diskRadial = length(diskUv);
        float diskAngle = atan(diskUv.y, diskUv.x);
        vec2 flowUv = rotate2D(uTime * uBlackHoleDiskSpinSpeed * 0.18) * diskUv;
        float diskNoise =
            fbm(flowUv * mix(2.0, 7.5, uBlackHoleDiskTurbulence) +
                vec2(uTime * 0.055, -uTime * 0.038));
        float fineNoise =
            fbm(flowUv * mix(9.0, 22.0, uBlackHoleDiskTurbulence) +
                vec2(-uTime * 0.030, uTime * 0.047));
        float filamentPhase =
            diskAngle * mix(10.0, 30.0, uBlackHoleDiskTurbulence) -
            log(max(diskRadial, 0.08)) * 11.0 -
            uTime * uBlackHoleDiskSpinSpeed * 3.6 + diskNoise * 6.2;
        float filaments =
            mix(0.28, 1.0,
                pow(clamp(0.5 + 0.5 * sin(filamentPhase), 0.0, 1.0),
                    mix(3.2, 0.72, uBlackHoleDiskTurbulence)));
        float darkLanes =
            smoothstep(0.18, 0.92, fineNoise) *
            (0.58 + 0.42 * smoothstep(1.2, 4.2, diskRadial));

        float diskInner = 1.12;
        float diskPeak = 1.55 + ringWidth * 0.25;
        float diskOuter = 4.35 + uBlackHoleDistortion * 1.15;
        float diskAA = max(fwidth(diskRadial) * 2.8, radialAA * 5.0);
        float radialWindow =
            smoothstep(diskInner, diskInner + diskAA * 1.8, diskRadial) *
            (1.0 - smoothstep(diskOuter, diskOuter + 0.55, diskRadial));
        float radialEnergy =
            exp(-max(diskRadial - diskPeak, 0.0) * 0.52) *
            (0.35 + 0.65 * exp(-pow((diskRadial - diskPeak) / 0.78, 2.0)));
        float diskThickness =
            (0.080 + ringWidth * 0.24) *
            (1.0 + smoothstep(1.5, diskOuter, diskRadial) * 0.45) *
            mix(0.72, 1.0, nearSide);
        float volumeRimThickness = diskThickness * mix(1.45, 2.20, nearSide);
        float diskCoreProfile =
            exp(-pow(abs(diskUv.y) / max(diskThickness, 0.012), 2.0));
        float diskRimProfile =
            exp(-pow((abs(diskUv.y) - volumeRimThickness) /
                         max(diskThickness * 0.55, 0.008),
                     2.0));
        float farSideInnerOcclusion =
            mix(1.0, smoothstep(1.05, 1.62, diskRadial), farSide * 0.92);
        float directDisk =
            (diskCoreProfile + diskRimProfile * 0.24 * nearSide) *
            radialWindow * radialEnergy * outsideShadow * farSideInnerOcclusion;

        float arcX = abs(diskBasisUv.x);
        float upperArcY =
            (0.33 + 0.095 * arcX + 0.016 * arcX * arcX +
             0.05 * uBlackHoleDistortion) *
            mix(0.78, 1.08, inclinationSin);
        float lowerArcY =
            (0.24 + 0.050 * arcX + 0.009 * arcX * arcX +
             0.03 * uBlackHoleDistortion) *
            mix(0.70, 0.96, inclinationSin);
        float arcThickness =
            (0.050 + ringWidth * 0.085) *
            (1.0 + arcX * 0.12) *
            mix(0.70, 1.10, inclinationSin);
        float arcRadialEnvelope =
            smoothstep(0.92, 1.05, shadowRadial) *
            (1.0 - smoothstep(3.55 + uBlackHoleDistortion * 0.55,
                              4.55 + uBlackHoleDistortion * 0.55,
                              shadowRadial)) *
            (1.0 - smoothstep(4.2, 5.4, arcX));
        float upperArc =
            exp(-pow((diskBasisUv.y - upperArcY) /
                         max(arcThickness, 0.012),
                     2.0)) *
            arcRadialEnvelope * outsideShadow * 1.10;
        float lowerArc =
            exp(-pow((diskBasisUv.y + lowerArcY) /
                         max(arcThickness * 0.86, 0.010),
                     2.0)) *
            arcRadialEnvelope * 0.92 * outsideShadow;
        float secondaryArcs = max(upperArc, lowerArc);

        float dopplerSide =
            smoothstep(-2.5, 2.5, diskBasisUv.x);
        float doppler = mix(1.0 - uBlackHoleDopplerBoost * 0.42,
                            1.0 + uBlackHoleDopplerBoost * 1.65,
                            dopplerSide);
        float diskTemperature =
            pow(clamp((diskOuter - diskRadial) / max(diskOuter - diskInner, 0.1),
                      0.0, 1.0),
                0.72);
        diskTemperature = max(diskTemperature,
                              exp(-pow((diskRadial - diskPeak) / 0.45, 2.0)) *
                                  0.65);

        vec3 userAccretion =
            inverseACES(clamp(uBlackHoleColor, vec3(0.0), vec3(1.0)));
        vec3 dustyOuter = userAccretion * vec3(0.72, 0.25, 0.08);
        vec3 amberMid = userAccretion * vec3(1.28, 0.68, 0.30);
        vec3 whiteHot = mix(userAccretion * vec3(1.55, 1.05, 0.70),
                            vec3(1.45, 1.26, 0.92),
                            0.58);
        vec3 diskColor = mix(dustyOuter, amberMid,
                             smoothstep(0.18, 0.68, diskTemperature));
        diskColor = mix(diskColor, whiteHot,
                        smoothstep(0.62, 1.0, diskTemperature));
        float filamentEnergy =
            mix(0.42, 1.18, filaments) * mix(0.62, 1.08, darkLanes);
        float clumpBreakup =
            mix(0.50, 1.10,
                smoothstep(0.26, 0.86,
                           fbm(flowUv * 3.8 +
                               vec2(uTime * 0.080, uTime * 0.025))));
        diskColor *= filamentEnergy * clumpBreakup * max(doppler, 0.0) *
                     depthLight;

        float diskMask = max(directDisk, secondaryArcs * 1.05);
        float innerDiskGlow =
            exp(-pow((diskRadial - diskPeak) /
                         max(0.12 + ringWidth * 0.28, 0.05),
                     2.0)) *
            outsideShadow * smoothstep(0.55, 1.0, abs(diskUv.x)) *
            mix(0.62, 1.18, nearSide);
        float diskGain = log(1.0 + max(uBlackHoleRingIntensity, 0.0)) * 1.35;
        mapped += diskColor *
                  (diskMask + innerDiskGlow * 0.20) *
                  diskGain * uExposure;

        float foregroundWindow =
            (1.0 - outsideShadow) *
            smoothstep(0.38, 0.72, shadowRadial) *
            smoothstep(0.45, 1.35, abs(diskBasisUv.x)) * nearSide;
        float foregroundBand =
            exp(-pow((diskUv.y + 0.018) / max(diskThickness * 0.92, 0.018),
                     2.0)) *
            radialWindow * radialEnergy * foregroundWindow;
        foregroundDiskAdd =
            diskColor * foregroundBand * diskGain * uExposure * 0.92;

        float nearRim =
            diskRimProfile * radialWindow * radialEnergy * nearSide *
            outsideShadow * (0.35 + 0.65 * smoothstep(1.15, 3.0, diskRadial));
        mapped += diskColor * nearRim * diskGain * uExposure * 0.36;

        float photonWidth = max(0.010 + ringWidth * 0.055, radialAA * 1.4);
        float photonRing =
            exp(-pow((shadowRadial - 1.018) / photonWidth, 2.0)) *
            (0.72 + 0.28 * doppler);
        float einsteinRing =
            exp(-pow((shadowRadial - (1.28 + uBlackHoleDistortion * 0.18)) /
                         max(0.09 + ringWidth * 0.15, radialAA * 2.0),
                     2.0)) *
            lensMask;
        vec3 photonColor =
            mix(vec3(0.55, 0.70, 1.45), vec3(1.25, 0.96, 0.62), dopplerSide) *
            (userAccretion + vec3(0.08, 0.10, 0.16));
        mapped += photonColor * photonRing *
                  max(uBlackHolePhotonRingIntensity, 0.0) * uExposure;
        mapped += mix(lensedSky, photonColor, 0.55) * einsteinRing *
                  (0.06 + 0.10 * uBlackHoleStarLensIntensity);

        float corona =
            exp(-pow((shadowRadial - 1.42) /
                         max(0.44 + ringWidth * 0.55, 0.10),
                     2.0)) *
            outsideShadow;
        float broadHalo =
            exp(-pow(shadowRadial /
                         (2.8 + uBlackHoleDistortion * 3.2),
                     2.0)) *
            outsideShadow;
        vec3 coronaColor =
            mix(userAccretion * vec3(0.45, 0.62, 1.35),
                userAccretion * vec3(1.65, 0.82, 0.32),
                0.70 + diskNoise * 0.30);
        mapped += coronaColor *
                  (corona * max(uBlackHoleCoronaIntensity, 0.0) * 0.52 +
                   broadHalo * max(uBlackHoleHaloIntensity, 0.0) * 0.28) *
                  uExposure;

        float jetAxis = 1.0 - smoothstep(0.018, 0.13, abs(diskBasisUv.x));
        float jetReach = 1.0 - smoothstep(1.12, 7.8, abs(diskBasisUv.y));
        float jetGap = smoothstep(1.08, 1.55, abs(diskBasisUv.y));
        float jetCore =
            pow(max(jetAxis * jetReach * jetGap, 0.0), 2.4) * outsideShadow;
        vec3 jetColor =
            inverseACES(vec3(0.45, 0.68, 1.0)) *
            jetCore *
            (0.75 + 0.25 * sin(uTime * 2.0 + abs(diskBasisUv.y) * 4.0));
        mapped += jetColor * max(uBlackHoleJetIntensity, 0.0) * 0.18 *
                  uExposure;
    } else {
        mapped = applySkyClouds(mapped, worldDir, sunDir);
    }

    if (!uUseBlackHole && solidSky) {
        float sunDot = max(dot(worldDir, sunDir), 0.0);
        float lowSunDamp = mix(0.22, 1.0, smoothstep(0.02, 0.34, sunDir.y));
        float discSoftness = max(uSunDiscSoftness, 0.0001);
        float disc = smoothstep(uSunSize - discSoftness, uSunSize, sunDot);
        float haloPower = mix(220.0, 2.4, clamp(uSunHaloSize, 0.0, 1.0));
        float halo = pow(sunDot, haloPower) * (1.0 - disc * 0.45);
        float horizon = pow(1.0 - clamp(abs(worldDir.y), 0.0, 1.0), 2.0);
        float rayNoise = 0.72 + 0.28 * fbm(dirToEquirectUV(worldDir) * 42.0 +
                                           vec2(uTime * 0.015, 0.0));
        float rays = pow(sunDot, max(uSunRaySharpness, 1.0)) * horizon * rayNoise;
        vec3 sunHdr = inverseACES(clamp(uSunColor, vec3(0.0), vec3(1.0)));
        mapped += sunHdr * (disc * max(uSunDiscIntensity, 0.0) +
                            halo * max(uSunHaloIntensity, 0.0) * lowSunDamp +
                            rays * max(uSunRaysIntensity, 0.0) * lowSunDamp);
    }

    // Night sky: stars + milky way + horizon glow
    if (uNightFactor > 0.001) {
        float stars = starField(worldDir) * uStarIntensity;
        vec2 uv = dirToEquirectUV(worldDir);
        float band = exp(-pow(dot(worldDir, normalize(vec3(0.2, 0.7, 0.1))), 2.0) * 8.0);
        float dust = fbm(uv * 18.0 + vec2(0.0, uTime * 0.002));
        float milky = band * dust * uMilkyWayIntensity;
        float horizon = pow(clamp(1.0 - abs(worldDir.y), 0.0, 1.0), 3.5);
        vec3 glow = uNightHorizonGlow * horizon;
        mapped += (stars + milky) * uNightFactor;
        mapped += glow * uNightFactor;
    }

    // Subtle night dithering to reduce banding
    if (uNightFactor > 0.001 && uNightDither > 0.0) {
        float d = (hash21(vUV * vec2(1024.0, 512.0)) - 0.5) * uNightDither;
        mapped += vec3(d);
    }

    if (uUseBlackHole) {
        // The event horizon is a final occluder so clouds, stars, halo, or disk
        // glow can never accidentally fill the black core back in.
        mapped *= 1.0 - eventShadowMask *
                           (0.24 + 0.22 * clamp(uBlackHoleShadowStrength, 0.0, 1.0));
        mapped = mix(mapped, vec3(0.0), clamp(eventHorizonMask, 0.0, 1.0));
        mapped += foregroundDiskAdd;
    }

    FragColor = vec4(mapped, 1.0);

}
