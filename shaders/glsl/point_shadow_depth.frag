#version 330 core
in vec3 vWorldPos;

uniform vec3 lightPos;
uniform float far_plane;

void main()
{
    float lightDistance = length(vWorldPos - lightPos);
    lightDistance = lightDistance / far_plane;
    gl_FragDepth = lightDistance;
}
