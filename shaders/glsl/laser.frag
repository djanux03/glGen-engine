#version 330 core
in float vSide;
in float vT;

out vec4 FragColor;

uniform vec3 uColor;
uniform float uIntensity;
uniform float uTime;
uniform float uPulse;
uniform bool uHit;

void main()
{
    float edge = 1.0 - abs(vSide);
    float core = smoothstep(0.18, 1.0, edge);
    float feather = pow(clamp(edge, 0.0, 1.0), 1.55);
    float startFade = smoothstep(0.0, 0.035, vT);
    float endFade = mix(1.0 - smoothstep(0.96, 1.0, vT) * 0.35,
                        1.0,
                        uHit ? 1.0 : 0.0);
    float flicker = 0.88 + 0.12 * sin(uTime * 44.0 - vT * 28.0);
    float fade = smoothstep(0.0, 0.18, uPulse);
    float alpha =
        clamp(feather * startFade * endFade * flicker * fade * 0.82, 0.0, 1.0);

    vec3 hotCore = mix(uColor, vec3(0.82, 0.96, 1.0), core * 0.72);
    vec3 color = hotCore * (uIntensity * mix(0.55, 1.2, core) *
                            mix(0.55, 1.2, fade));
    FragColor = vec4(color, alpha);
}
