#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec3 aNormal;

layout (location = 3) in mat4 aInstanceMatrix;

out vec2 TexCoord;
out vec3 FragPos;
out vec3 Normal;
out vec4 FragPosLightSpace;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform bool uInstanced;

uniform mat4 uLightSpaceMatrix;

void main()
{
    mat4 finalModel = uInstanced ? aInstanceMatrix : model;
    
    vec4 worldPos = finalModel * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;

    Normal = mat3(transpose(inverse(finalModel))) * aNormal;

    FragPosLightSpace = uLightSpaceMatrix * worldPos;

    TexCoord = aTexCoord;
    gl_Position = projection * view * worldPos;
}
