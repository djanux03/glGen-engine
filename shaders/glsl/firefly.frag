#version 330 core
in float vFade;
out vec4 FragColor;

uniform float uTime;
uniform float uIntensity;
uniform vec3 uColor;

void main()
{
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(p, p);
    float core = exp(-r2 * 6.0);
    float flicker = 0.75 + 0.25 * sin(uTime * 4.0 + r2 * 12.0);
    float alpha = core * vFade * flicker;
    vec3 col = uColor * uIntensity * core;
    FragColor = vec4(col, alpha);
}
