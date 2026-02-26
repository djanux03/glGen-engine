#version 330 core
out vec4 FragColor;

in vec2 TexCoord;
in vec3 FragPos;
in vec3 Normal;

uniform sampler2D texture1;

// Base material
uniform bool  uUseColor;
uniform vec4  uColor;

// Sun "visual knobs" mapped into lighting
uniform float uSunIntensity;
uniform float uAmbient;

// FX
uniform bool  uGlowPass;
uniform float uGlowStrength;
uniform float uTime;

// CLOUDS
uniform bool  uCloudPass;
uniform vec3  uCloudColor;
uniform float uCloudScale;
uniform float uCloudSpeed;
uniform float uCloudCover;
uniform float uCloudSoftness;
uniform float uCloudAlpha;
uniform float uCloudHeight;
uniform float uCloudThickness;
uniform float uCloudDensity;
uniform float uCloudLightAbsorption;
uniform float uCloudPhaseG;
uniform vec3  uCloudWind;

// Lighting / camera
uniform vec3  uSunColor;
uniform vec3  uCameraPos;

// Tuning
uniform float uSpecStrength;
uniform float uShininess;
uniform float uGamma;

// Point-shadow uniforms (SUN as point light with shadows)
uniform samplerCube shadowCube;     // texture unit 1
uniform vec3  uLightPos;            // sun position
uniform float uFarPlane;
uniform float uShadowStrength;

// NEW: Campfire point light (no shadows)
uniform bool  uHasFire;
uniform vec3  uFirePos;
uniform vec3  uFireColor;           // e.g. vec3(1.0, 0.45, 0.10)
uniform float uFireIntensity;       // e.g. 2.0
uniform float uFireConstant;        // e.g. 1.0
uniform float uFireLinear;          // e.g. 0.14
uniform float uFireQuadratic;       // e.g. 0.07
uniform float uFireFlicker;         // e.g. 0.15 (0..0.3 looks good)
uniform float uFireAmbient;        // small ambient boost amount (linear space)
uniform float uFireAmbientRadius;  // distance where it fades out

uniform sampler2D texDiffuse;
uniform sampler2D texNormal;
uniform sampler2D texRoughness;
uniform sampler2D texMetallic;

// ---------- helpers ----------
float rand(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = rand(i);
    float b = rand(i + vec2(1.0, 0.0));
    float c = rand(i + vec2(0.0, 1.0));
    float d = rand(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * noise(p);
        p *= 2.0;
        a *= 0.5;
    }
    return v;
}

// Optimization: Pre-defined sample offsets to avoid nested loops
vec3 gridSamplingDisk[20] = vec3[](
   vec3(1, 1, 1), vec3(1, -1, 1), vec3(-1, -1, 1), vec3(-1, 1, 1),
   vec3(1, 1, -1), vec3(1, -1, -1), vec3(-1, -1, -1), vec3(-1, 1, -1),
   vec3(1, 1, 0), vec3(1, -1, 0), vec3(-1, -1, 0), vec3(-1, 1, 0),
   vec3(1, 0, 1), vec3(-1, 0, 1), vec3(1, 0, -1), vec3(-1, 0, -1),
   vec3(0, 1, 1), vec3(0, -1, 1), vec3(0, -1, -1), vec3(0, 1, -1)
);

float ShadowPoint(vec3 fragPos, vec3 lightPos)
{
    vec3 fragToLight = fragPos - lightPos;
    float currentDepth = length(fragToLight);

    float shadow = 0.0;
    float bias = 0.05;
    float viewDistance = length(uCameraPos - FragPos);
    
    // Auto-tune radius: sharper shadows when close, softer when far
    float diskRadius = (1.0 + (viewDistance / uFarPlane)) / 25.0;

    // OPTIMIZATION: Dynamic sample count based on distance from camera
    int samples = 20;
    if (viewDistance > 40.0) samples = 10;
    if (viewDistance > 80.0) samples = 4;

    // Using a stable subset of our 20-sample array
    for(int i = 0; i < samples; ++i)
    {
        float closestDepth = texture(shadowCube, fragToLight + gridSamplingDisk[i] * diskRadius).r;
        closestDepth *= uFarPlane;
        if(currentDepth - bias > closestDepth)
            shadow += 1.0;
    }
    
    shadow /= float(samples);
    return clamp(shadow * uShadowStrength, 0.0, 1.0); 
}

// Gamma helpers
vec3 toLinear(vec3 srgb) { return pow(max(srgb, vec3(0.0)), vec3(uGamma)); }
vec3 toSRGB(vec3 lin)    { return pow(max(lin,  vec3(0.0)), vec3(1.0 / uGamma)); }

vec3 toneMapReinhard(vec3 c) { return c / (c + vec3(1.0)); }

void main()
{
    // ---------------- GLOW PASS ----------------
    if (uGlowPass)
    {
        vec2 uv = TexCoord - vec2(0.5);
        float r = length(uv);

        float mask = 1.0 - smoothstep(0.48, 0.50, r);
        if (mask <= 0.001) discard;

        float core = smoothstep(0.18, 0.00, r);
        float glow = smoothstep(0.52, 0.10, r);

        float tt = uTime * 0.12;
        vec2 q = uv * 3.5;
        q += 0.35 * vec2(fbm(q + tt), fbm(q - tt));
        float nn = fbm(q * 2.2 + vec2(tt, -tt));
        float flicker = mix(0.75, 1.35, nn);

        float outer = (1.0 - core);
        float intensity = (3.0 * core + 1.2 * glow * flicker * outer) * uGlowStrength;

        vec3 hot  = vec3(1.00, 0.98, 0.85);
        vec3 warm = vec3(1.00, 0.65, 0.20);
        vec3 col  = mix(warm, hot, core);

        vec4 c;
        c.rgb = col * intensity * mask;
        c.a   = (0.50 * glow + 0.35 * core) * mask;

        c.rgb = toneMapReinhard(c.rgb);
        c.rgb = toSRGB(c.rgb);

        FragColor = c;
        return;
    }

    // ---------------- CLOUD PASS ----------------
    if (uCloudPass)
    {
        vec3 viewDir = normalize(FragPos - uCameraPos);
        float dirY = max(viewDir.y, 0.05);
        float distToTop = uCloudThickness / dirY;
        
        int numSteps = 16;
        float stepSize = distToTop / float(numSteps);
        vec3 rayStep = viewDir * stepSize;
        
        vec3 p = FragPos;
        float T = 1.0; 
        vec3 cloudLit = vec3(0.0);
        
        for (int i = 0; i < numSteps; i++) {
            vec2 uv = p.xz * 0.01 * uCloudScale;
            uv += uCloudWind.xz * uTime * uCloudSpeed;
            
            float n = fbm(uv);
            float d = smoothstep(1.0 - uCloudCover, 1.0 - uCloudCover + uCloudSoftness, n);
            d *= uCloudDensity;
            
            if (d > 0.0) {
                float lightTransmittance = exp(-d * uCloudLightAbsorption);
                vec3 S = uSunColor * uSunIntensity * lightTransmittance + vec3(0.2); 
                
                cloudLit += T * S * d * stepSize * uCloudColor;
                T *= exp(-d * stepSize);
                if (T < 0.01) break;
            }
            p += rayStep;
        }
        
        float finalAlpha = (1.0 - T) * uCloudAlpha;
        
        // Soft edge fade
        float edgeFade = 1.0 - smoothstep(0.3, 0.5, length(TexCoord - vec2(0.5)));
        finalAlpha *= edgeFade;

        if (finalAlpha <= 0.01) discard;

        cloudLit = toneMapReinhard(cloudLit);
        cloudLit = toSRGB(cloudLit);

        FragColor = vec4(cloudLit, finalAlpha);
        return;
    }

    // ---------------- BASE COLOR ----------------
    vec4 baseColor = uUseColor ? uColor : texture(texDiffuse, TexCoord);

    // ---------------- LIGHTING + SHADOWS ----------------
    vec3 N = normalize(Normal);
    vec3 V = normalize(uCameraPos - FragPos);

    vec3 albedo = toLinear(baseColor.rgb);

    // Ambient (no direct-light scaling)
    vec3 ambient = albedo * uAmbient;

    // ---------- SUN (shadowed point light) ----------
    vec3  Ls = normalize(uLightPos - FragPos);
    vec3  Hs = normalize(Ls + V);
    float NdotLs = max(dot(N, Ls), 0.0);

    // OPTIMIZATION: Early out! If the fragment is facing entirely away from the sun,
    // don't bother sampling the 4K shadow map 20 times. It's already in shadow.
    float shadow = 1.0;
    if (NdotLs > 0.0) {
        shadow = ShadowPoint(FragPos, uLightPos);
    }

    vec3 sun = uSunColor * uSunIntensity;

    vec3 sunDiffuse  = albedo * (NdotLs * sun);
    float sunSpecTerm = pow(max(dot(N, Hs), 0.0), uShininess);
    vec3  sunSpecular = uSpecStrength * sunSpecTerm * sun;

    vec3 lit = ambient + (1.0 - shadow) * (sunDiffuse + sunSpecular);

    // ---------- FIRE (non-shadowed point light) ----------
    if (uHasFire)
    {
        vec3  toFire = uFirePos - FragPos;
        float dist   = length(toFire);
        vec3  Lf     = (dist > 0.0001) ? (toFire / dist) : vec3(0.0, 1.0, 0.0);

        // Classic attenuation (constant + linear*d + quadratic*d^2)
        float attenuation = 1.0 / (uFireConstant + uFireLinear * dist + uFireQuadratic * (dist * dist));

        // Small flicker (multiply intensity a bit)
        float flicker = 1.0 + uFireFlicker * sin(uTime * 17.0 + FragPos.x * 3.0 + FragPos.z * 2.0);

        vec3 fire = uFireColor * (uFireIntensity * attenuation * flicker);

        float NdotLf = max(dot(N, Lf), 0.0);
        vec3 fireDiffuse = albedo * (NdotLf * fire);

        vec3 Hf = normalize(Lf + V);
        float fireSpecTerm = pow(max(dot(N, Hf), 0.0), uShininess);
        vec3  fireSpecular = uSpecStrength * fireSpecTerm * fire;

        // Local ambient lift (subtle): helps nearby surfaces not go fully black.
        float amb = clamp(1.0 - dist / uFireAmbientRadius, 0.0, 1.0);
        amb = amb * amb; // smooth falloff
        lit += albedo * (uFireAmbient * amb) * uFireColor;
        lit += fireDiffuse + fireSpecular;
    }

    // Tonemap + gamma
    lit = toneMapReinhard(lit);
    lit = toSRGB(lit);

    // --- FOG CALCULATION ---
    float dist = length(uCameraPos - FragPos);
    
    // Exponential fog formula: 1 / e^(distance * density)^2
    float fogFactor = 1.0 / exp(pow(dist * 0.005, 2.0)); // 0.005 matches your C++ density
    fogFactor = clamp(fogFactor, 0.0, 1.0);

    // Mix the scene color with the fog color based on distance
    lit = mix(vec3(0.5, 0.6, 0.7), lit, fogFactor); 

    FragColor = vec4(lit, baseColor.a);
}
