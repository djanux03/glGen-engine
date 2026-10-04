#version 450

// Denoises full-resolution SSAO while preserving small grass-root pockets.
// A plain box blur would bleed AO across
// silhouette edges (a rock's occlusion smearing onto the sky/terrain behind
// it), so each tap is weighted by how close its view-space depth is to the
// center pixel's -- a bilateral upsample, using the same full-res depth
// prepass SSAO itself reads.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAO;    // full-res raw AO
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
    ivec2 size=textureSize(uDepth,0);
    float depth=texelFetch(uDepth,clamp(ivec2(vUV*vec2(size)),ivec2(0),size-1),0).r;
    if(depth>=1.0){outColor=vec4(1.0);return;}
    float centerZ = viewZFromDepth(vUV, depth);
    // A small bilateral footprint keeps occlusion local to roots instead of
    // spreading it across the bare soil between bunches.
    vec2 texel = 1.0 / vec2(textureSize(uAO, 0));
    // Match raw-AO texel centers so no interpolation leaks contact shadows
    // across foreground edges before the bilateral depth rejection.
    vec2 base=(floor(vUV/texel)+.5)*texel;
    float sum = 0.0;
    float wSum = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec2 sampleUV = base + vec2(x, y) * texel;
            ivec2 tap=clamp(ivec2(sampleUV*vec2(size)),ivec2(0),size-1);
            float sampleZ = viewZFromDepth(sampleUV, texelFetch(uDepth,tap,0).r);
            // exp(-|Δz|) in view-space meters -- ~1m of depth separation
            // already cuts a tap's weight to ~13%, sharp enough to stop
            // bleeding across most silhouette edges without a hard cutoff
            // (which would itself reintroduce aliasing at the boundary).
            float spatial=exp(-float(x*x+y*y)*.65);
            float w=spatial*exp(-abs(sampleZ-centerZ)/max(.12,abs(centerZ)*.012));
            sum += texelFetch(uAO,tap,0).r * w;
            wSum += w;
        }
    }
    outColor = vec4(vec3(sum / max(wSum, 1e-4)), 1.0);
}
