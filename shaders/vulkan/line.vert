#version 450
// line.vert -- editor debug lines (grid, origin axes, collider outlines).
//
// Deliberately standalone: a push constant carrying viewProj is all it needs,
// so this pipeline has no descriptor sets at all and can be recorded anywhere
// in the scene pass without binding the frame UBO, bindless table or TLAS.
//
// Note what this shader does NOT do: the distance fade. That has to happen per
// FRAGMENT (see line.frag). A grid line is hundreds of metres long, so fading
// by vertex distance judges the whole line by its two far endpoints -- past
// the fade radius both ends reach zero alpha and the entire line vanishes,
// including the stretch passing directly under the camera.

layout(push_constant) uniform Push {
    mat4 viewProj;
    // xyz = camera world position, w = fade distance in metres.
    vec4 camPosFade;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vWorldPos;

void main() {
    gl_Position = pc.viewProj * vec4(inPos, 1.0);
    vColor = inColor;
    vWorldPos = inPos;
}
