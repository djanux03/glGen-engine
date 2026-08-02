#version 450

// Bright-pass extract: samples the full-res HDR scene color at half-res UV
// (the existing linear sampler does a cheap ~2x2 box downsample for free),
// keeps only the portion above a soft-knee threshold. Feeds the separable
// blur (bloomBlur.frag).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uHdr;

layout(push_constant) uniform Push {
    float threshold;
    float knee;
} pc;

void main() {
    // Clamp: negatives would survive the blur and subtract light in the
    // composite; the upper bound kills fireflies -- a handful of extreme
    // pixels (specular hits, shader bugs) would otherwise bloom into a
    // screen-wide flood.
    vec3 color = clamp(texture(uHdr, vUV).rgb, vec3(0.0), vec3(64.0));
    float brightness = max(color.r, max(color.g, color.b));
    // Soft knee: smoothly ramps the contribution in over [threshold-knee,
    // threshold+knee] instead of a hard cutoff that pops as brightness
    // crosses the line.
    float contribution =
        smoothstep(pc.threshold - pc.knee, pc.threshold + pc.knee, brightness);
    outColor = vec4(color * contribution, 1.0);
}
