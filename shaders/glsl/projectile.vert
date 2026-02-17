#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aCenter;
layout (location = 2) in vec3 aColor;
layout (location = 3) in float aAlpha;

out vec3 vColor;
out vec2 vUV;
out float vAlpha;
out vec3 vCenter;

uniform mat4 view;
uniform mat4 projection;
uniform float uSize;

void main()
{
    vColor = aColor;
    vAlpha = aAlpha;
    vCenter = aCenter;
    vUV = aPos.xy + vec2(0.5);

    vec3 camRight = vec3(view[0][0], view[1][0], view[2][0]);
    vec3 camUp    = vec3(view[0][1], view[1][1], view[2][1]);

    vec3 worldPos = aCenter + (camRight * aPos.x + camUp * aPos.y) * uSize;
    gl_Position = projection * view * vec4(worldPos, 1.0);
}
