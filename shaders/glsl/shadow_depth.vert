#version 330 core
layout (location = 0) in vec3 aPos;

layout (location = 3) in mat4 aInstanceMatrix;
layout (location = 7) in vec4 aTerrainChunk0;

uniform mat4 model;
uniform mat4 uLightSpaceMatrix;
uniform bool uInstanced;
uniform bool uGpuTerrainPass;
uniform sampler2DArray uTerrainHeightPages;
uniform float uTerrainSkirtDepth;

void main()
{
    if (uGpuTerrainPass)
    {
        vec2 uv = clamp(aPos.xz, vec2(0.0), vec2(1.0));
        float layer = aTerrainChunk0.w;
        float h = texture(uTerrainHeightPages, vec3(uv, layer)).r;
        h -= aPos.y * uTerrainSkirtDepth;
        vec4 worldPos = vec4(aTerrainChunk0.x + aPos.x * aTerrainChunk0.z,
                             h,
                             aTerrainChunk0.y + aPos.z * aTerrainChunk0.z,
                             1.0);
        gl_Position = uLightSpaceMatrix * worldPos;
        return;
    }

    mat4 finalModel = uInstanced ? aInstanceMatrix : model;
    gl_Position = uLightSpaceMatrix * finalModel * vec4(aPos, 1.0);
}
