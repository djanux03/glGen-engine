#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require

// Terrain shading: directional light + ray-traced shadow (the model casts onto
// the terrain via a shadow ray against the TLAS), green tint, UV from world XZ.
layout(location = 0) in vec3 vNormalWS;
layout(location = 1) in vec3 vWorldPos;
layout(location = 2) in float vViewZ;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(set = 1, binding = 0) uniform FrameData {
    mat4 viewProj;
    mat4 view;
    mat4 lightSpace[3];
    vec4 lightDir;
    vec4 cascadeSplits;
} uFrame;

layout(set = 2, binding = 0) uniform accelerationStructureEXT uTLAS;

layout(push_constant) uniform Push {
    uint textureIndex;
} pc;

float traceShadow(vec3 origin, vec3 dir) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTLAS,
                          gl_RayFlagsTerminateOnFirstHitEXT |
                              gl_RayFlagsOpaqueEXT |
                              gl_RayFlagsSkipClosestHitShaderEXT,
                          0xFFu, origin, 0.01, dir, 1000.0);
    rayQueryProceedEXT(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) ==
                   gl_RayQueryCommittedIntersectionNoneEXT
               ? 1.0
               : 0.0;
}

void main() {
    vec3 N = normalize(vNormalWS);
    vec3 L = normalize(-uFrame.lightDir.xyz);
    float ndl = max(dot(N, L), 0.0);

    float vis = 1.0;
    if (ndl > 0.0)
        vis = traceShadow(vWorldPos + N * 0.02, L);

    vec2 uv = vWorldPos.xz * 0.5;
    vec3 tex = texture(uTextures[nonuniformEXT(pc.textureIndex)], uv).rgb;
    tex *= vec3(0.45, 0.62, 0.35);
    vec3 color = tex * (0.25 + 0.75 * ndl * vis);
    outColor = vec4(color, 1.0);
}
