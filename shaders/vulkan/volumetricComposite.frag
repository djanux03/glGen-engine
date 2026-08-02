#version 450
#extension GL_GOOGLE_include_directive : require

// Composite the half-res volumetric scatter buffer onto the full-res HDR
// scene target (additive blend -- this pass ADDS in-scattered light; the
// surface shaders' analytic fog already handles extinction + skylight).
//
// Depth-aware upsample: 4 bilinear-neighborhood taps of the half-res
// scatter, each weighted by how closely its stored march distance (alpha
// channel) matches this full-res pixel's own marchable distance --
// prevents god rays from bleeding across tree/terrain silhouettes.
layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uScatter; // half-res, a=march dist
layout(set = 1, binding = 0) uniform sampler2D uDepth;   // full-res

layout(push_constant) uniform Push {
    mat4 invViewProj;
    vec4 camPosMaxDist; // xyz = camera world pos, w = volumetric max distance
} pc;

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;

    float depth = texture(uDepth, uv).r;
    float surfaceDist = 1e9;
    if (depth < 1.0) {
        vec4 posW = pc.invViewProj * vec4(vNdc, depth, 1.0);
        surfaceDist = distance(posW.xyz / posW.w, pc.camPosMaxDist.xyz);
    }
    float fullEnd = min(surfaceDist, max(pc.camPosMaxDist.w, 1.0));

    vec2 scatterSize = vec2(textureSize(uScatter, 0));
    vec2 texel = 1.0 / scatterSize;
    // Snap to the half-res grid, then take the 4 nearest texel centers.
    vec2 base = (floor(uv * scatterSize - 0.5) + 0.5) * texel;
    vec2 f = clamp((uv - base) * scatterSize, 0.0, 1.0);

    vec4 s00 = texture(uScatter, base);
    vec4 s10 = texture(uScatter, base + vec2(texel.x, 0.0));
    vec4 s01 = texture(uScatter, base + vec2(0.0, texel.y));
    vec4 s11 = texture(uScatter, base + texel);

    float w00 = (1.0 - f.x) * (1.0 - f.y) / (abs(s00.a - fullEnd) + 0.35);
    float w10 = f.x * (1.0 - f.y) / (abs(s10.a - fullEnd) + 0.35);
    float w01 = (1.0 - f.x) * f.y / (abs(s01.a - fullEnd) + 0.35);
    float w11 = f.x * f.y / (abs(s11.a - fullEnd) + 0.35);

    vec3 scatter = (s00.rgb * w00 + s10.rgb * w10 + s01.rgb * w01 +
                    s11.rgb * w11) /
                   max(w00 + w10 + w01 + w11, 1e-5);

    outColor = vec4(max(scatter, vec3(0.0)), 0.0);
}
