#version 450
#extension GL_GOOGLE_include_directive : require

// Terrain-only vertex stage -- a copy of mesh.vert plus a 4th vertex
// attribute (MeshVertex::terrainParams: curvature, rockMask, wForest,
// wMountain, all 0..1) that only the terrain pipeline binds. Kept as a
// separate shader file rather than added to mesh.vert itself because
// mesh.vert is shared by every prop/scene pipeline, which only declare 3
// vertex attributes (pos/normal/uv) in their VkPipelineVertexInputStateCreateInfo
// -- unconditionally adding a 4th input there would leave those pipelines'
// vertex shader expecting an attribute the pipeline never binds (undefined
// per spec, validation-layer-unfriendly). This file is used ONLY by
// VulkanRenderer::createTerrainChunkPipeline(); the shared depth prepass
// still uses mesh.vert unchanged (it never needed terrainParams -- position
// only), reading from the exact same (wider) vertex buffer via its own
// 3-attribute pipeline state, same as before R1.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inTerrainParams;

layout(location = 0) out vec3 vNormalWS;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out float vViewZ;
layout(location = 4) out vec4 vTerrainParams;

#include "frameData.glsl"

layout(push_constant) uniform Push {
    mat4 model;        // per-instance transform
    uint textureIndex; // used in the fragment stage
} pc;

void main() {
    vec4 world = pc.model * vec4(inPos, 1.0);
    gl_Position = uFrame.viewProj * world;
    vNormalWS = mat3(pc.model) * inNormal;
    vUV = inUV;
    vWorldPos = world.xyz;
    vViewZ = -(uFrame.view * world).z; // positive distance in front of camera
    vTerrainParams = inTerrainParams;
}
