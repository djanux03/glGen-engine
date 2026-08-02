#version 450

// Denoises the raw SSAO pass's per-pixel sampling noise (its hemisphere
// kernel is randomized per-pixel via a hash, not a fixed rotation texture,
// so the raw output is noisy without this) AND upsamples it: uAO is
// half-resolution (see createSceneTargets' mSSAOImages), this pass's output
// (mSSAOBlurImages) is full-resolution, sampled directly by the scene pass.
// A plain box blur across that resolution jump would bleed AO across
// silhouette edges (a rock's occlusion smearing onto the sky/terrain behind
// it), so each tap is weighted by how close its view-space depth is to the
// center pixel's -- a bilateral upsample, using the same full-res depth
// prepass SSAO itself reads.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAO;    // half-res raw AO
layout(set = 1, binding = 0) uniform sampler2D uDepth; // full-res depth

layout(push_constant) uniform Push {
    mat4 invProj; // NDC+depth -> view-space position, for the depth weight
} pc;

float viewZFromDepth(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = pc.invProj * clip;
    return view.z / view.w;
}

void main() {
    float centerZ = viewZFromDepth(vUV, texture(uDepth, vUV).r);
    // Step by the half-res AO texture's texel size -- walking the same grid
    // it was rendered at, not the full-res depth's finer grid.
    vec2 texel = 1.0 / vec2(textureSize(uAO, 0));
    float sum = 0.0;
    float wSum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2 sampleUV = vUV + vec2(x, y) * texel;
            float sampleZ = viewZFromDepth(sampleUV, texture(uDepth, sampleUV).r);
            // exp(-|Δz|) in view-space meters -- ~1m of depth separation
            // already cuts a tap's weight to ~13%, sharp enough to stop
            // bleeding across most silhouette edges without a hard cutoff
            // (which would itself reintroduce aliasing at the boundary).
            float w = exp(-abs(sampleZ - centerZ) * 2.0);
            sum += texture(uAO, sampleUV).r * w;
            wSum += w;
        }
    }
    outColor = vec4(vec3(sum / max(wSum, 1e-4)), 1.0);
}
