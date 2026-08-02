#version 450

// Separable 9-tap gaussian blur, one direction per draw (drive it with
// direction=(1,0) then (0,1) for a full 2-pass blur -- see VulkanRenderer's
// createBloomBlurPipeline / drawFrame's bloom passes).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uSource;

layout(push_constant) uniform Push {
    vec2 direction; // texel-space step direction, e.g. (1,0) or (0,1)
} pc;

void main() {
    vec2 texel = pc.direction / vec2(textureSize(uSource, 0));
    // Normalized 9-tap gaussian weights (sigma ~1.6).
    float weights[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

    vec3 result = texture(uSource, vUV).rgb * weights[0];
    for (int i = 1; i < 5; ++i) {
        vec2 offset = texel * float(i);
        result += texture(uSource, vUV + offset).rgb * weights[i];
        result += texture(uSource, vUV - offset).rgb * weights[i];
    }
    outColor = vec4(result, 1.0);
}
