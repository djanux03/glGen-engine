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

    if (uUseSolidSky)
    {
        // Gradient + a little horizon haze for depth.
        float t = clamp(worldDir.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 col = mix(uSkyHorizon, uSkyTop, t);
        float haze = pow(clamp(1.0 - abs(worldDir.y), 0.0, 1.0), 2.5);
        col += vec3(0.04, 0.03, 0.02) * haze;
        mapped = pow(max(col, vec3(0.0)), vec3(1.0 / uGamma));
    }
    else
    {
        vec2 uv = dirToEquirectUV(worldDir);
        vec3 hdr = texture(uHDR, uv).rgb;
        mapped = vec3(1.0) - exp(-hdr * uExposure);
        mapped = pow(max(mapped, vec3(0.0)), vec3(1.0 / uGamma));
    }

    // Lightweight sky clouds (no raymarch; only a few noise samples).
    if (uSkyCloudsEnabled)
    {
        float proj = max(worldDir.y + 0.35, 0.12);
        vec2 cloudUV = worldDir.xz / proj * uSkyCloudScale;
        vec2 wind = vec2(uTime * uSkyCloudSpeed, uTime * uSkyCloudSpeed * 0.73);
        float n1 = fbm(cloudUV + wind);
        float n2 = fbm(cloudUV * 1.87 - wind * 0.6 + vec2(9.3, -4.7));
        float cloudShape = mix(n1, n2, 0.35);
        float cloudMask = smoothstep(uSkyCloudCoverage, uSkyCloudCoverage + uSkyCloudSoftness, cloudShape);
        float horizonFade = smoothstep(-0.10, 0.20, worldDir.y);
        float cloudAlpha = clamp(cloudMask * horizonFade * uSkyCloudDensity, 0.0, 1.0);
        mapped = mix(mapped, uSkyCloudColor, cloudAlpha);
    }

    // Richer sun: disc + halo + soft radial rays.
    vec3 sunDir = normalize(-uSunDir);
    float sunAlignment = dot(worldDir, sunDir);
    float sunDisc = smoothstep(uSunSize - 0.00025, uSunSize + 0.00015, sunAlignment);
    float radial = acos(clamp(sunAlignment, -1.0, 1.0)); // radians from sun center
    float halo = exp(-radial * 24.0) * (1.0 - sunDisc);

    vec3 upRef = abs(sunDir.y) > 0.95 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 tSun = normalize(cross(upRef, sunDir));
    vec3 bSun = normalize(cross(sunDir, tSun));
    vec2 local = vec2(dot(worldDir, tSun), dot(worldDir, bSun));
    float ang = atan(local.y, local.x);
    float rayShape = pow(abs(sin(ang * 4.0 + uTime * 0.12)), 24.0);
    float rayMask = smoothstep(0.35, 0.06, radial);
    float rays = rayShape * rayMask * (1.0 - sunDisc);

    vec3 sunContribution =
        uSunColor * (sunDisc * uSunDiscIntensity +
                     halo * uSunHaloIntensity +
                     rays * uSunRaysIntensity);
    mapped += sunContribution;

    FragColor = vec4(mapped, 1.0);

}
