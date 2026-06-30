#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require

// Directional lighting × bindless texture, with HARDWARE RAY-TRACED shadows:
// a shadow ray is traced toward the sun against the scene TLAS (set 2). No
// shadow maps, no cascades — sharp, correct contact shadows.
layout(location = 0) in vec3 vNormalWS;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in float vViewZ;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uTextures[];

layout(set = 1, binding = 0) uniform FrameData {
    mat4 viewProj;
    mat4 view;
    vec4 lightDir;
    vec4 terrain;
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
                          0xFFu, origin, 0.001, dir, 1000.0);
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

    vec3 tex = texture(uTextures[nonuniformEXT(pc.textureIndex)], vUV).rgb;
    vec3 color = tex * (0.2 + 0.8 * ndl * vis);

    // Aerial/distance fog toward the sky (day/night aware via sun elevation).
    float day = smoothstep(-0.1, 0.2, -uFrame.lightDir.y);
    vec3 fogColor = mix(vec3(0.03, 0.04, 0.07), vec3(0.55, 0.65, 0.78), day);
    float fog = 1.0 - exp(-max(vViewZ - 2.0, 0.0) * 0.05);
    color = mix(color, fogColor, clamp(fog, 0.0, 0.9));

    outColor = vec4(color, 1.0);
}
