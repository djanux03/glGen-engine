#version 330 core
layout (location = 0) in vec3 aPos;

layout (location = 3) in mat4 aInstanceMatrix;

uniform mat4 model;
uniform mat4 uLightSpaceMatrix;
uniform bool uInstanced;

void main()
{
    mat4 finalModel = uInstanced ? aInstanceMatrix : model;
    gl_Position = uLightSpaceMatrix * finalModel * vec4(aPos, 1.0);
}
