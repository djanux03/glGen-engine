#version 450

// Screen-space ambient occlusion (Crysis/LearnOpenGL-style hemisphere
// kernel). Samples a depth-only Z-prepass (see VulkanRenderer's
// createDepthPrepassPipelines) -- reconstructs view-space position from
// depth, and the surface normal from screen-space derivatives of that
// position (no separate normal G-buffer). Outputs grayscale occlusion;
// mesh.frag/terrain.frag sample the blurred result and multiply it into
// their ambient term only (never direct light).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDepth;

layout(push_constant) uniform Push {
    mat4 invProj; // NDC+depth -> view-space position
    mat4 proj;    // view-space -> NDC, to re-project kernel samples
    float radius;
    float bias;
    float strength;
    float pad;
} pc;

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

vec3 viewPosFromDepth(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = pc.invProj * clip;
    return view.xyz / view.w;
}

void main() {
    float depth = texture(uDepth, vUV).r;
    if (depth >= 1.0) {
        outColor = vec4(1.0);
        return;
    }

    vec3 P = viewPosFromDepth(vUV, depth);
    // View space here looks down -Z; the surface normal must face the
    // camera (positive-ish Z component after the cross product).
    vec3 N = normalize(cross(dFdx(P), dFdy(P)));
    if (N.z < 0.0)
        N = -N;

    vec3 rvec = normalize(vec3(hash13(P) * 2.0 - 1.0,
                               hash13(P.yzx + 7.0) * 2.0 - 1.0, 0.0) +
                          1e-4);
    vec3 tangent = normalize(rvec - N * dot(rvec, N));
    vec3 bitangent = cross(N, tangent);
    mat3 TBN = mat3(tangent, bitangent, N);

    const int kSamples = 16;
    float occlusion = 0.0;
    for (int i = 0; i < kSamples; ++i) {
        float fi = float(i) / float(kSamples);
        vec3 h = vec3(hash13(P + fi) * 2.0 - 1.0,
                     hash13(P.zxy + fi) * 2.0 - 1.0,
                     hash13(P.yxz + fi));
        vec3 sampleDir = normalize(TBN * normalize(h + 1e-4));
        // Concentrate samples closer to the origin (denser near the surface).
        float scale = mix(0.1, 1.0, fi * fi);
        vec3 samplePos = P + sampleDir * pc.radius * scale;

        vec4 offset = pc.proj * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        vec2 sampleUV = offset.xy * 0.5 + 0.5;

        float sampleDepthNDC = texture(uDepth, sampleUV).r;
        vec3 occluderViewPos = viewPosFromDepth(sampleUV, sampleDepthNDC);

        float rangeCheck =
            smoothstep(0.0, 1.0, pc.radius / max(abs(P.z - occluderViewPos.z), 1e-4));
        occlusion += (occluderViewPos.z >= samplePos.z + pc.bias ? 1.0 : 0.0) *
                    rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(kSamples)) * pc.strength;
    outColor = vec4(vec3(clamp(occlusion, 0.0, 1.0)), 1.0);
}
