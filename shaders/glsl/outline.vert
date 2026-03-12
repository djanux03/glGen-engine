#version 410 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

layout (location = 3) in mat4 aInstanceMatrix;

// Uniforms
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform bool uInstanced;

uniform float outlineScale = 1.02; // Scale factor for the outline

void main()
{
    mat4 finalModel = uInstanced ? aInstanceMatrix : model;
    // Simple object-space scaling to avoid cracks on flat-shaded meshes
    vec4 scaledPos = vec4(aPos * outlineScale, 1.0);
    gl_Position = projection * view * finalModel * scaledPos;
}
