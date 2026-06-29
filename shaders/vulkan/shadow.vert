#version 450

// Depth-only shadow caster. One draw per cascade; the push constant holds that
// cascade's world->light-clip matrix (the scene is static, model = identity).
// No fragment shader: the pass writes only depth.
layout(location = 0) in vec3 inPos;

layout(push_constant) uniform Push {
    mat4 lightViewProj;
} pc;

void main() {
    gl_Position = pc.lightViewProj * vec4(inPos, 1.0);
}
