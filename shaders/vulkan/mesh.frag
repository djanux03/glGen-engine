#version 450
#extension GL_EXT_nonuniform_qualifier : require

// Directional lighting × bindless texture, attenuated by cascaded shadow maps.
layout(location = 0) in vec3 vNormalWS;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in float vViewZ;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(set = 1, binding = 0) uniform FrameData {
    mat4 viewProj;
    mat4 view;
    mat4 lightSpace[3];
    vec4 lightDir;
    vec4 cascadeSplits;
} uFrame;

layout(set = 2, binding = 0) uniform sampler2DArrayShadow uShadowMap;

layout(push_constant) uniform Push {
    uint textureIndex;
} pc;

int pickCascade(float viewZ) {
    if (viewZ < uFrame.cascadeSplits.x) return 0;
    if (viewZ < uFrame.cascadeSplits.y) return 1;
    return 2;
}

float sampleShadow(int cascade, vec3 worldPos, float ndl) {
    vec4 lc = uFrame.lightSpace[cascade] * vec4(worldPos, 1.0);
    vec3 ndc = lc.xyz / lc.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float depthRef = ndc.z; // Vulkan depth is already [0,1]
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || depthRef > 1.0)
        return 1.0; // outside this cascade -> treat as lit

    float bias = max(0.0015 * (1.0 - ndl), 0.0006);
    depthRef -= bias;

    // 3x3 PCF on top of hardware comparison filtering.
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0).xy);
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            shadow += texture(uShadowMap,
                              vec4(uv + vec2(x, y) * texel, float(cascade), depthRef));
    return shadow / 9.0;
}

void main() {
    vec3 N = normalize(vNormalWS);
    vec3 L = normalize(-uFrame.lightDir.xyz);
    float ndl = max(dot(N, L), 0.0);

    int cascade = pickCascade(vViewZ);
    float vis = sampleShadow(cascade, vWorldPos, ndl);

    vec3 tex = texture(uTextures[nonuniformEXT(pc.textureIndex)], vUV).rgb;
    vec3 color = tex * (0.2 + 0.8 * ndl * vis);
    outColor = vec4(color, 1.0);
}
