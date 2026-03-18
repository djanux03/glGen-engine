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

vec3 toneMapReinhard(vec3 c) { return c / (c + vec3(1.0)); }
vec3 toSRGB(vec3 lin) { return pow(max(lin, vec3(0.0)), vec3(1.0 / 2.2)); }

void main()
{
    vec3 viewDir = normalize(FragPos - uCameraPos);
    float dirY = max(viewDir.y, 0.05);
    float distToTop = uCloudThickness / dirY;

    int numSteps = 16;
    float stepSize = distToTop / float(numSteps);
    vec3 rayStep = viewDir * stepSize;

    vec3 p = FragPos;
    float T = 1.0;
    vec3 cloudLit = vec3(0.0);

    for (int i = 0; i < numSteps; i++) {
        vec2 uv = p.xz * 0.01 * uCloudScale;
        uv += uCloudWind.xz * uTime * uCloudSpeed;

        float n = fbm(uv);
        float d = smoothstep(1.0 - uCloudCover, 1.0 - uCloudCover + uCloudSoftness, n);
        d *= uCloudDensity;

        if (d > 0.0) {
            float lightTransmittance = exp(-d * uCloudLightAbsorption);
            vec3 S = uSunColor * uSunIntensity * lightTransmittance + vec3(0.2);

            cloudLit += T * S * d * stepSize * uCloudColor;
            T *= exp(-d * stepSize);
            if (T < 0.01) break;
        }
        p += rayStep;
    }

    float finalAlpha = (1.0 - T) * uCloudAlpha;

    float edgeFade = 1.0 - smoothstep(0.3, 0.5, length(TexCoord - vec2(0.5)));
    finalAlpha *= edgeFade;

    if (finalAlpha <= 0.01) discard;

    cloudLit = toneMapReinhard(cloudLit);
    cloudLit = toSRGB(cloudLit);

    FragColor = vec4(cloudLit, finalAlpha);
}
