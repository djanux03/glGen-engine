// volumetric_fog.frag
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D depthTex;
uniform vec3 sunColor;
uniform vec3 uLightDir;
uniform float nearPlane;
uniform float farPlane;
uniform float fogDensity;
uniform float lightExposure;
uniform float lightDecay;
uniform float lightWeight;
uniform int sampleCount;
uniform float sunVisible;
uniform float sunPosX;
uniform float sunPosY;
uniform float uTime;

float linearizeDepth(float d) {
    float z = d * 2.0 - 1.0;
    return (2.0 * nearPlane * farPlane) /
           max(farPlane + nearPlane - z * (farPlane - nearPlane), 0.0001);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    float depth = texture(depthTex, TexCoords).r;
    float linearDepth = linearizeDepth(depth);
    float depth01 = clamp(linearDepth / farPlane, 0.0, 1.0);

    vec3 viewDirApprox = normalize(vec3(TexCoords * 2.0 - 1.0, -1.0));
    vec3 sunDir = normalize(-uLightDir);
    float phase = pow(max(dot(viewDirApprox, sunDir), 0.0), 6.0);

    // Base aerial perspective fog term.
    float fog = 1.0 - exp(-depth01 * (fogDensity * 45.0));
    vec3 fogColor = mix(vec3(0.52, 0.60, 0.70), vec3(0.72, 0.78, 0.86), phase);
    vec3 volumetric = fogColor * fog * 0.35;

    // Screen-space radial light shafts from sun position.
    vec2 sunUV = vec2(sunPosX, sunPosY);
    vec2 delta = (TexCoords - sunUV) / float(max(sampleCount, 1));
    float illumDecay = 1.0;
    vec2 coord = TexCoords;
    vec3 shafts = vec3(0.0);

    int taps = clamp(sampleCount, 8, 64);
    float jitter = hash12(TexCoords + uTime * 0.05) * 0.002;
    for (int i = 0; i < 64; ++i) {
        if (i >= taps) break;
        coord -= delta + vec2(jitter);

        if (coord.x < 0.0 || coord.x > 1.0 || coord.y < 0.0 || coord.y > 1.0)
            continue;

        float d = texture(depthTex, coord).r;
        float occlusion = (d >= 0.9995) ? 1.0 : 0.18;
        shafts += sunColor * occlusion * illumDecay * lightWeight;
        illumDecay *= lightDecay;
    }

    shafts *= lightExposure * sunVisible * phase;
    volumetric += shafts;

    FragColor = vec4(volumetric, 1.0);
}
