#version 330 core

in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uRaymarchTex;
uniform float uExposure;

vec3 aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(uRaymarchTex, vUV).rgb * max(uExposure, 0.001);
    FragColor = vec4(aces(hdr), 1.0);
}
