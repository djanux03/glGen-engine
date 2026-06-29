#version 450

// Post-process / tonemap pass. Samples the linear-HDR scene target, applies
// exposure + ACES tonemapping, then encodes to ~sRGB for the UNORM swapchain.
// This is the seam where exposure/tonemap/gamma live in the new renderer.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uHdr;

layout(push_constant) uniform Push {
    float exposure;
} pc;

vec3 acesFilmic(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(uHdr, vUV).rgb * pc.exposure;
    vec3 mapped = acesFilmic(hdr);
    vec3 srgb = pow(mapped, vec3(1.0 / 2.2));
    outColor = vec4(srgb, 1.0);
}
