#version 330 core
in vec3 vColor;
in vec2 vUV;
in float vAlpha;
in vec3 vCenter;
out vec4 FragColor;

uniform float uTime;
uniform int uSmokePass;

float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);

    float a = hash(i + vec2(0.0, 0.0));
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Cheaper fbm: 3 octaves
float fbm3(vec2 p)
{
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 3; ++i)
    {
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

void main()
{
    vec2 p = vUV * 2.0 - 1.0;
    float r2 = dot(p, p);

    // Very cheap early reject (prevents "rectangle" fill cost)
    if (r2 > 1.0) discard;

    // Safe radial alpha (no undefined smoothstep)
    float radial = 1.0 - smoothstep(0.25, 1.0, r2);

    // bullets: keep simple
    if (uSmokePass == 0)
    {
        float a = radial * vAlpha;
        if (a <= 0.01) discard;
        FragColor = vec4(vColor, a);
        return;
    }

    // smoke age
    float age = clamp(1.0 - vAlpha, 0.0, 1.0);
    float seed = hash(vCenter.xz * 0.17);

    // simple drift
    vec2 uv = vUV;
    uv.y += uTime * 0.10 + age * (0.35 + 0.25 * seed);

    // 1 warp fbm + 1 density fbm (much cheaper than multiple)
    float w = fbm3(uv * 3.0 + seed * 10.0 + uTime * 0.05);
    uv += (w - 0.5) * mix(0.06, 0.14, age);

    float d = fbm3(uv * 6.0 + seed * 13.0 + vec2(uTime * 0.10, -uTime * 0.06));

    // erosion: more breakup over life
    float thr = mix(0.25, 0.75, age);
    float erode = smoothstep(thr, thr + 0.20, d);

    float alpha = radial * erode * vAlpha;
    if (alpha <= 0.01) discard;

    vec3 col = mix(vec3(0.20), vec3(0.65), age);
    FragColor = vec4(col, alpha);
}
