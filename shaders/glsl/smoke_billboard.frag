#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform float uOpacity; // 0..1

float hash21(vec2 p){
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}
float noise(vec2 p){
    vec2 i=floor(p), f=fract(p);
    float a=hash21(i), b=hash21(i+vec2(1,0)), c=hash21(i+vec2(0,1)), d=hash21(i+vec2(1,1));
    vec2 u=f*f*(3.0-2.0*f);
    return mix(a,b,u.x) + (c-a)*u.y*(1.0-u.x) + (d-b)*u.x*u.y;
}
float fbm(vec2 p){
    float v=0.0, a=0.5;
    // fBM layers [web:571]
    for(int i=0;i<5;i++){ v += a*noise(p); p*=2.0; a*=0.5; }
    return v;
}

void main()
{
    vec2 uv = vUV;

    // Smoke is above the fire: bias it upward in UV space
    uv.y = clamp((uv.y - 0.10) / 0.90, 0.0, 1.0);

    float t = uTime;

    // Rising drift + swirl via domain warping [web:571]
    vec2 p = uv * vec2(2.0, 3.5) + vec2(0.15*sin(t*0.3), -t*0.35);
    vec2 w = vec2(fbm(p + 5.1), fbm(p + 9.7));
    uv += (w - 0.5) * vec2(0.18, 0.12);

    float d = fbm(uv * vec2(4.0, 6.5) + vec2(0.0, -t*0.25));

    // Soft column that expands as it rises
    float x = uv.x - 0.5;
    float width = mix(0.12, 0.35, smoothstep(0.0, 1.0, uv.y));
    float column = 1.0 - smoothstep(width, width + 0.18, abs(x));

    // More transparent at the top, denser near bottom
    float fadeTop = smoothstep(1.0, 0.15, uv.y);

    // Wispy thresholding
    float density = smoothstep(0.40, 0.75, d) * column * fadeTop;

    // Slightly warm near the bottom, cooler higher up
    vec3 smokeCol = mix(vec3(0.20, 0.19, 0.18), vec3(0.08, 0.09, 0.10), smoothstep(0.0, 1.0, uv.y));

    float alpha = density * uOpacity;

    if (alpha < 0.01) discard;
    FragColor = vec4(smokeCol, alpha);
}
