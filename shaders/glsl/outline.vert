#version 410 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

// Uniforms
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

uniform float outlineScale = 1.02; // Scale factor for the outline

void main()
{
    // Simple object-space scaling to avoid cracks on flat-shaded meshes
    vec4 scaledPos = vec4(aPos * outlineScale, 1.0);
    gl_Position = projection * view * model * scaledPos;
}
