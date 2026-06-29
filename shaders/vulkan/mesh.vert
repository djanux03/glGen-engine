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
    mat4 lightSpace[3];
    vec4 lightDir;       // world space, xyz
    vec4 cascadeSplits;  // x,y,z = far view-depth of cascades 0,1,2
} uFrame;

void main() {
    vec4 world = vec4(inPos, 1.0);
    gl_Position = uFrame.viewProj * world;
    vNormalWS = inNormal;
    vUV = inUV;
    vWorldPos = inPos;
    vViewZ = -(uFrame.view * world).z; // positive distance in front of camera
}
