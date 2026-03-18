#version 330 core
layout(location = 0) in vec3 aPos;

uniform mat4 view;
uniform mat4 projection;
uniform float uSize;

out float vFade;

void main()
{
    vec4 viewPos = view * vec4(aPos, 1.0);
    gl_Position = projection * viewPos;
    float dist = max(length(viewPos.xyz), 1.0);
    float size = uSize * (260.0 / dist);
    gl_PointSize = clamp(size, 2.0, 16.0);
    vFade = clamp(1.0 - dist / 120.0, 0.0, 1.0);
}
