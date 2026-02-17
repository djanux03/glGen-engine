#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform float uTime;
uniform float uIntensity;   // 0..1+
uniform float uDetail = 1.0; // optional: 0.5..2.0

// ---------------- noise utils ----------------
float hash21(vec2 p){
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

float noise(vec2 p){
    vec2 i = floor(p), f = fract(p);
    float a = hash21(i);
    float b = hash21(i + vec2(1,0));
    float c = hash21(i + vec2(0,1));
    float d = hash21(i + vec2(1,1));
    vec2 u = f*f*(3.0-2.0*f);
    return mix(a,b,u.x) + (c-a)*u.y*(1.0-u.x) + (d-b)*u.x*u.y;
}

float fbm(vec2 p){
    float v = 0.0;
    float a = 0.5;
    for(int i=0;i<6;i++){
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

// Ridged noise gives sharper “tongues” of flame
float ridged(vec2 p){
    float n = fbm(p);
    n = 1.0 - abs(2.0*n - 1.0);
    return n*n;
}

// Curl-ish 2D flow field from fbm gradient (cheap “swirl”)
vec2 curl2(vec2 p){
    float e = 0.08;
    float n1 = fbm(p + vec2(0.0, e));
    float n2 = fbm(p - vec2(0.0, e));
    float n3 = fbm(p + vec2(e, 0.0));
    float n4 = fbm(p - vec2(e, 0.0));
    float dx = (n3 - n4) / (2.0*e);
    float dy = (n1 - n2) / (2.0*e);
    return vec2(dy, -dx); // rotate gradient 90 degrees
}

vec3 fireRamp(float t){
    t = clamp(t, 0.0, 1.0);
    vec3 deep = vec3(0.02, 0.00, 0.00);
    vec3 red  = vec3(0.95, 0.10, 0.02);
    vec3 org  = vec3(1.00, 0.55, 0.06);
    vec3 yel  = vec3(1.00, 0.90, 0.55);
    vec3 hot  = vec3(1.00, 0.99, 0.90);

    vec3 c = mix(deep, red, smoothstep(0.00, 0.25, t));
    c = mix(c, org,  smoothstep(0.15, 0.60, t));
    c = mix(c, yel,  smoothstep(0.45, 0.85, t));
    c = mix(c, hot,  smoothstep(0.80, 1.00, t));
    return c;
}

void main()
{
    // Base coordinates
    vec2 uv = vUV;
    float t = uTime;

    // --- Shape: narrower at top, wavy edges ---
    float y = uv.y;
    float x = uv.x - 0.5;

    float w = mix(0.34, 0.05, smoothstep(0.0, 1.0, y));
    float edgeSoft = 0.10 + 0.05 * uDetail;

    // Edge wobble so silhouette isn’t static
    float wob = (fbm(vec2(x*3.0, y*4.0) + vec2(0.0, -t*1.2)) - 0.5) * (0.10 + 0.12*y);
    x += wob;

    float shape = 1.0 - smoothstep(w, w + edgeSoft, abs(x));

    // Keep base tighter and stronger
    float basePinch = 1.0 - smoothstep(0.00, 0.14, y) * 0.35;
    shape *= basePinch;

    // --- Turbulent motion: swirl field + domain warping ---
    vec2 p = uv * vec2(2.4, 7.8) + vec2(0.0, -t * 1.8);

    vec2 flow = curl2(p * (1.0 + 0.35*uDetail));
    uv += flow * (0.06 + 0.10*y) * (0.7 + 0.6*uDetail);

    // 2nd warp for higher-frequency “licks”
    vec2 q = vec2(ridged(uv*vec2(4.0, 10.0) + vec2(0.0, -t*2.6)),
                  fbm  (uv*vec2(6.0, 12.0) + vec2(9.2, -t*3.2)));
    uv += (q - 0.5) * vec2(0.10, 0.06) * (0.2 + y) * uDetail;

    // --- Intensity field: mix smooth + ridged noise ---
    float nSmooth = fbm(uv * vec2(2.8, 10.5) + vec2(0.0, -t * 2.3));
    float nRidge  = ridged(uv * vec2(3.8, 13.5) + vec2(2.0, -t * 3.1));

    // Fade out upward, but keep base hot
    float baseHot = smoothstep(0.0, 0.18, y);
    float fadeTop = smoothstep(1.10, 0.08, y);

    // “Fuel pockets” (brighter blobs) around mid-height
    float pockets = smoothstep(0.45, 0.90, fbm(uv*vec2(3.0, 5.0) + vec2(11.0, -t*0.8)));
    pockets *= smoothstep(0.15, 0.65, y) * (1.0 - smoothstep(0.65, 1.0, y));

    float intensity = mix(nSmooth, nRidge, 0.55) * shape * fadeTop;
    intensity = pow(max(intensity, 0.0), 1.7);
    intensity *= (0.65 + 1.00 * baseHot);
    intensity += 0.35 * pockets * shape;
    intensity *= uIntensity;

    // --- Color / glow ---
    float core = smoothstep(0.35, 1.00, intensity);
    float glow = pow(clamp(intensity, 0.0, 1.0), 0.55);

    vec3 col = fireRamp(intensity);
    col *= (1.6 + 3.2 * glow);          // bright emissive feel
    col += vec3(1.0, 0.85, 0.55) * core * 0.65;

    // --- Embers (sparks) ---
    float emberBand = smoothstep(0.05, 0.35, y) * (1.0 - smoothstep(0.70, 1.0, y));
    float emberN = fbm(uv * vec2(25.0, 60.0) + vec2(0.0, -t * 4.2));
    float embers = smoothstep(0.86, 0.97, emberN) * emberBand * shape;
    col += embers * vec3(3.0, 2.2, 1.4);

    // Alpha (additive blend uses alpha as “how much to add”)
    float alpha = clamp(glow * 1.25, 0.0, 1.0);
    alpha *= mix(0.55, 1.0, shape);

    // Extra cutouts for “tongues”
    float cut = smoothstep(0.30, 0.95, fbm(uv * vec2(7.0, 14.0) + 13.0));
    alpha *= mix(0.55, 1.0, cut);

    if (alpha < 0.02) discard;
    FragColor = vec4(col, alpha);
}
