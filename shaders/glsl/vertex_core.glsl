#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec3 aNormal;

layout (location = 3) in mat4 aInstanceMatrix;
layout (location = 7) in vec4 aTerrainChunk0;
layout (location = 8) in vec4 aTerrainChunk1;

out vec2 TexCoord;
out vec3 FragPos;
out vec3 Normal;
out vec4 FragPosLightSpace;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform bool uInstanced;

uniform mat4 uLightSpaceMatrix;
uniform bool uGpuTerrainPass;
uniform sampler2DArray uTerrainHeightPages;
uniform sampler2DArray uTerrainBiomePages;
uniform float uTerrainSkirtDepth;

void main()
{
    if (uGpuTerrainPass)
    {
        vec2 uv = clamp(aPos.xz, vec2(0.0), vec2(1.0));
        float worldSize = aTerrainChunk0.z;
        float layer = aTerrainChunk0.w;
        float sampleCount = max(aTerrainChunk1.x, 2.0);
        float texelStep = 1.0 / max(sampleCount - 1.0, 1.0);

        float h = texture(uTerrainHeightPages, vec3(uv, layer)).r;
        h -= aPos.y * uTerrainSkirtDepth;

        vec4 worldPos = vec4(aTerrainChunk0.x + aPos.x * worldSize,
                             h,
                             aTerrainChunk0.y + aPos.z * worldSize,
                             1.0);

        float hL = texture(uTerrainHeightPages, vec3(clamp(uv - vec2(texelStep, 0.0), vec2(0.0), vec2(1.0)), layer)).r;
        float hR = texture(uTerrainHeightPages, vec3(clamp(uv + vec2(texelStep, 0.0), vec2(0.0), vec2(1.0)), layer)).r;
        float hD = texture(uTerrainHeightPages, vec3(clamp(uv - vec2(0.0, texelStep), vec2(0.0), vec2(1.0)), layer)).r;
        float hU = texture(uTerrainHeightPages, vec3(clamp(uv + vec2(0.0, texelStep), vec2(0.0), vec2(1.0)), layer)).r;

        FragPos = worldPos.xyz;
        Normal = normalize(vec3(-(hR - hL), 2.0 * worldSize * texelStep, -(hU - hD)));
        FragPosLightSpace = uLightSpaceMatrix * worldPos;
        TexCoord = vec2(aPos.x, texture(uTerrainBiomePages, vec3(uv, layer)).r);
        gl_Position = projection * view * worldPos;
        return;
    }

    mat4 finalModel = uInstanced ? aInstanceMatrix : model;
    
    vec4 worldPos = finalModel * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;

    Normal = mat3(transpose(inverse(finalModel))) * aNormal;

    FragPosLightSpace = uLightSpaceMatrix * worldPos;

    TexCoord = aTexCoord;
    gl_Position = projection * view * worldPos;
}
