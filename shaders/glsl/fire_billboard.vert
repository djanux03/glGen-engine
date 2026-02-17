#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;

out vec2 vUV;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec3 uCameraPos; // (not used here, but handy if you want)

// rotation around local Y (radians)
uniform float uYaw;

mat2 rot(float a) {
    float c = cos(a), s = sin(a);
    return mat2(c, -s, s,  c);
}

void main()
{
    vUV = aUV;

    // rotate quad vertices in local space so we can draw cross-planes
    vec3 p = aPos;
    vec2 xz = rot(uYaw) * vec2(p.x, p.z);
    p.x = xz.x;
    p.z = xz.y;

    gl_Position = projection * view * model * vec4(p, 1.0);
}
