#version 330 core
out vec4 FragColor;

in vec2 TexCoord;
in vec3 FragPos;

uniform vec3  uCloudColor;
uniform float uCloudScale;
uniform float uCloudSpeed;
uniform float uCloudCover;
uniform float uCloudSoftness;
uniform float uCloudAlpha;
uniform float uCloudHeight;
uniform float uCloudThickness;
uniform float uCloudDensity;
uniform float uCloudLightAbsorption;
uniform float uCloudPhaseG;
uniform vec3  uCloudWind;

uniform vec3  uCameraPos;
uniform vec3  uSunColor;
uniform float uSunIntensity;
uniform vec3  uSunDir;
uniform float uTime;

float rand(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = rand(i);
    float b = rand(i + vec2(1.0, 0.0));
    float c = rand(i + vec2(0.0, 1.0));
    float d = rand(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

float hgPhase(float cosTheta, float g)
{
    float gg = g * g;
    return (1.0 - gg) / max(pow(1.0 + gg - 2.0 * g * cosTheta, 1.5), 0.001);
}

float cloudDensityAt(vec3 p)
{
    float h = clamp((p.y - uCloudHeight) / max(uCloudThickness, 0.001), 0.0, 1.0);
    float heightShape = smoothstep(0.02, 0.24, h) *
                        (1.0 - smoothstep(0.72, 1.0, h));
    vec2 uv = p.xz * 0.006 * max(uCloudScale, 0.001);
    uv += uCloudWind.xz * uTime * uCloudSpeed;

    vec2 warp = vec2(fbm(uv * 0.42 + vec2(4.2, -1.7)),
                     fbm(uv * 0.37 + vec2(-8.1, 6.9))) - 0.5;
    uv += warp * 1.8;

    float broad = fbm(uv);
    float puffs = fbm(uv * 2.7 + vec2(12.0, -5.0));
    float erosion = fbm(uv * 7.0 + vec2(-2.0, 17.0));
    float shape = broad * 0.62 + puffs * 0.32 - erosion * 0.16;
    float threshold = mix(0.74, 0.30, clamp(uCloudCover, 0.0, 1.0));
    float d = smoothstep(threshold, threshold + max(uCloudSoftness, 0.015),
                         shape);
    return d * heightShape * max(uCloudDensity, 0.0);
}

void main()
{
    vec3 viewDir = normalize(FragPos - uCameraPos);
    if (viewDir.y <= 0.015)
        discard;

    float dirY = max(viewDir.y, 0.05);
    float distToTop = min(uCloudThickness / dirY, 220.0);

    int numSteps = 24;
    float stepSize = distToTop / float(numSteps);
    vec3 rayStep = viewDir * stepSize;

    vec3 p = FragPos;
    float T = 1.0;
    vec3 cloudLit = vec3(0.0);
    vec3 toSun = normalize(-uSunDir);
    float phase = hgPhase(max(dot(viewDir, toSun), 0.0), clamp(uCloudPhaseG, 0.0, 0.92));
    vec3 ambientSky = uCloudColor * vec3(0.38, 0.44, 0.56);

    for (int i = 0; i < numSteps; i++) {
        float d = cloudDensityAt(p);

        if (d > 0.0) {
            float opticalDepth = d * stepSize * 0.030;
            float lightTransmittance = exp(-d * uCloudLightAbsorption);
            float powder = 1.0 - exp(-d * 2.8);
            vec3 direct = uSunColor * uSunIntensity *
                          (0.25 + 0.75 * phase) *
                          lightTransmittance * (0.55 + 0.45 * powder);
            vec3 S = ambientSky + direct;

            cloudLit += T * S * opticalDepth * uCloudColor;
            T *= exp(-opticalDepth);
            if (T < 0.01) break;
        }
        p += rayStep;
    }

    float finalAlpha = clamp((1.0 - T) * uCloudAlpha, 0.0, 0.88);

    float edgeFade = 1.0 - smoothstep(0.3, 0.5, length(TexCoord - vec2(0.5)));
    finalAlpha *= edgeFade;

    if (finalAlpha <= 0.01) discard;

    vec3 color = cloudLit / max(1.0 - T, 0.08);
    FragColor = vec4(color, finalAlpha);
}
