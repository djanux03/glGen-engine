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

vec3 customSkyGradient(vec3 worldDir)
{
    float vertical = clamp(worldDir.y * 0.5 + 0.5, 0.0, 1.0);
    vertical = pow(vertical, max(uSkyGradientPower, 0.001));
    vec3 sky = inverseACES(mix(uSkyHorizon, uSkyTop, vertical)) * uExposure;

    float horizon = pow(1.0 - clamp(abs(worldDir.y), 0.0, 1.0), 2.3);
    sky += inverseACES(uSkyHorizon) * (horizon * max(uSkyHorizonGlow, 0.0));
    return sky;
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

    bool solidSky = uUseSolidSky;
    if (solidSky)
    {
        vec3 customSky = customSkyGradient(worldDir);
        float lowSunDamp = mix(0.34, 1.0, smoothstep(0.02, 0.34, sunDir.y));
        float atmosphereBlend =
            clamp(uSkyAtmosphereStrength, 0.0, 1.0) * lowSunDamp;
        if (atmosphereBlend > 0.001) {
            vec3 atmosphereSky = calculateAtmosphere(worldDir, sunDir);
            mapped = mix(customSky, atmosphereSky, atmosphereBlend);
        } else {
            mapped = customSky;
        }
    }
    else
    {
        vec2 uv = dirToEquirectUV(worldDir);
        vec3 hdr = texture(uHDR, uv).rgb;
        mapped = hdr * uExposure;
    }

    mapped = applySkyClouds(mapped, worldDir, sunDir);

    if (solidSky) {
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

    FragColor = vec4(mapped, 1.0);

}
