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

    // Slope/height based terrain materials: sand -> grass, snow on peaks,
    // rock on steep slopes.
    float slope = 1.0 - clamp(N.y, 0.0, 1.0);
    float h = vWorldPos.y;
    vec3 sand  = vec3(0.60, 0.54, 0.37);
    vec3 grass = vec3(0.22, 0.42, 0.16);
    vec3 rock  = vec3(0.33, 0.29, 0.25);
    vec3 snow  = vec3(0.90, 0.93, 0.97);
    vec3 albedo = mix(sand, grass, smoothstep(-0.52, -0.38, h));
    albedo = mix(albedo, snow, smoothstep(0.30, 0.60, h));
    albedo = mix(albedo, rock, smoothstep(0.35, 0.65, slope));
    // Subtle detail break-up from the bindless texture.
    float det = texture(uTextures[nonuniformEXT(pc.textureIndex)],
                        vWorldPos.xz * 0.35).r;
    albedo *= 0.8 + 0.4 * det;

    vec3 color = albedo * (0.25 + 0.75 * ndl * vis);

    // Aerial/distance fog toward the sky (day/night aware via sun elevation).
    float day = smoothstep(-0.1, 0.2, -uFrame.lightDir.y);
    vec3 fogColor = mix(vec3(0.03, 0.04, 0.07), vec3(0.55, 0.65, 0.78), day);
    float fog = 1.0 - exp(-max(vViewZ - 2.0, 0.0) * 0.05);
    color = mix(color, fogColor, clamp(fog, 0.0, 0.9));

    outColor = vec4(color, 1.0);
}
