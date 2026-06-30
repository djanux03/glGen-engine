#version 450

// Scene vertex stage. Static scene (model = identity), so world position is the
// input position. Outputs the view-space depth used to pick a shadow cascade.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vNormalWS;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out float vViewZ;

layout(set = 1, binding = 0) uniform FrameData {
    mat4 viewProj;
    mat4 view;
    vec4 lightDir;
    vec4 terrain; // x=amplitude y=frequency z=octaves w=seed
} uFrame;

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
}
