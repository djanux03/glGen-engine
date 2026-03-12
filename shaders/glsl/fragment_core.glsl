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

// PBR Tuning
uniform bool  uHasRoughnessMap;
uniform float uRoughness;
uniform bool  uHasMetallicMap;
uniform float uMetallic;
uniform bool  uHasNormalMap;
uniform bool  uHasAOMap;
uniform bool  uHasEmissiveMap;
uniform bool  uHasOpacityMap;
uniform float uAO;
uniform int   uRoughnessChannel;
uniform int   uMetallicChannel;
uniform int   uAOChannel;
uniform int   uOpacityChannel;
uniform bool  uRoughnessMapIsGloss;
uniform vec3  uEmissiveColor;
uniform float uEmissiveStrength;
uniform float uAlphaCutoff;
uniform float uGamma;

// Shadow uniforms (SUN as directional light with shadows)
uniform sampler2D shadowMap;        // texture unit 1
uniform mat4  uLightSpaceMatrix;
uniform vec3  uLightDir;            // sun dir
uniform float uFarPlane;
uniform float uShadowStrength;
uniform vec3  uFogColor;
uniform float uFogDensity;

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

// TERRAIN
uniform bool  uTerrainPass;
uniform bool  uTerrainMaterialEnabled;
uniform float uTerrainMacroScale;
uniform float uTerrainDetailScale;
uniform float uTerrainNormalDetailScale;
uniform float uTerrainNormalStrength;
uniform float uTerrainCliffStart;
uniform float uTerrainCliffEnd;
uniform float uTerrainSnowStart;
uniform float uTerrainSnowEnd;
uniform float uTerrainLowStart;
uniform float uTerrainLowEnd;
uniform float uTerrainMacroVariationStrength;
uniform float uTerrainCliffDesatStrength;
uniform vec3  uTerrainGrassA;
uniform vec3  uTerrainGrassB;
uniform vec3  uTerrainDirtA;
uniform vec3  uTerrainDirtB;
uniform vec3  uTerrainRockA;
uniform vec3  uTerrainRockB;
uniform vec3  uTerrainSandA;
uniform vec3  uTerrainSandB;
uniform vec3  uTerrainSnowA;
uniform vec3  uTerrainSnowB;
uniform float uTerrainRoughGrass;
uniform float uTerrainRoughDirt;
uniform float uTerrainRoughRock;
uniform float uTerrainRoughSand;
uniform float uTerrainRoughSnow;
uniform bool  uTerrainFlatGreenEnabled;
uniform vec3  uTerrainFlatGreenColor;

uniform sampler2D texDiffuse;
uniform sampler2D texNormal;
uniform sampler2D texRoughness;
uniform sampler2D texMetallic;
uniform sampler2D texAO;
uniform sampler2D texEmissive;
uniform sampler2D texOpacity;

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

// --- Directional Shadow Calculation (PCF) ---
float ShadowDirectional(vec4 fragPosLightSpace) {
    // perform perspective divide
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    // transform to [0,1] range
    projCoords = projCoords * 0.5 + 0.5;
    
    // keep the shadow at 0.0 when outside the far_plane region of the light's frustum.
    if(projCoords.z > 1.0)
        return 0.0;
        
    // get depth of current fragment from light's perspective
    float currentDepth = projCoords.z;
    
    // check whether current frag pos is in shadow
    vec3 normal = normalize(Normal);
    vec3 lightDir = normalize(-uLightDir);
    float bias = max(0.005 * (1.0 - dot(normal, lightDir)), 0.001);
    
    // PCF
    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    for(int x = -1; x <= 1; ++x)
    {
        for(int y = -1; y <= 1; ++y)
        {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r; 
            shadow += currentDepth - bias > pcfDepth  ? 1.0 : 0.0;        
        }    
    }
    shadow /= 9.0;
    
    return shadow * uShadowStrength;
}

// Gamma helpers
vec3 toLinear(vec3 srgb) { return pow(max(srgb, vec3(0.0)), vec3(uGamma)); }
vec3 toSRGB(vec3 lin)    { return pow(max(lin,  vec3(0.0)), vec3(1.0 / uGamma)); }

vec3 toneMapReinhard(vec3 c) { return c / (c + vec3(1.0)); }

const float PI = 3.14159265359;

// PBR: Normal Distribution (GGX)
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float num = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return num / max(denom, 0.0000001);
}

// PBR: Geometry (Smith)
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;

    float num = NdotV;
    float denom = NdotV * (1.0 - k) + k;
	
    return num / denom;
}
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2  = GeometrySchlickGGX(NdotV, roughness);
    float ggx1  = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// PBR: Fresnel (Schlick)
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float sampleChannelValue(vec4 sampleValue, int channel) {
    if (channel == 1) return sampleValue.g;
    if (channel == 2) return sampleValue.b;
    if (channel == 3) return sampleValue.a;
    return sampleValue.r;
}

float biomeWeight(float biomeV, float target) {
    return max(1.0 - abs(biomeV - target), 0.0);
}

float triplanarNoise(vec3 worldPos, vec3 worldNormal, float scale) {
    vec3 n = abs(normalize(worldNormal));
    vec3 w = pow(n, vec3(4.0));
    float sumW = w.x + w.y + w.z + 0.0001;
    w /= sumW;

    float nx = noise(worldPos.yz * scale);
    float ny = noise(worldPos.xz * scale);
    float nz = noise(worldPos.xy * scale);
    return nx * w.x + ny * w.y + nz * w.z;
}

vec3 sampleNormalWS(vec3 baseNormal) {
    vec3 N = normalize(baseNormal);
    if (!uHasNormalMap) return N;

    vec3 tangentNormal = texture(texNormal, TexCoord).xyz * 2.0 - 1.0;

    vec3 dp1 = dFdx(FragPos);
    vec3 dp2 = dFdy(FragPos);
    vec2 duv1 = dFdx(TexCoord);
    vec2 duv2 = dFdy(TexCoord);

    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    vec3 rawT = dp1 * duv2.y - dp2 * duv1.y;

    if (abs(det) < 0.000001 || dot(rawT, rawT) < 0.000001) {
        return N;
    }

    vec3 T = rawT - N * dot(N, rawT);
    if (dot(T, T) < 0.000001) {
        return N;
    }
    T = normalize(T);

    vec3 B = cross(N, T);
    if (dot(B, B) < 0.000001) {
        return N;
    }
    B = normalize(B);
    if (det < 0.0) B *= -1.0;

    mat3 TBN = mat3(T, B, N);
    return normalize(TBN * tangentNormal);
}

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
    bool useTerrainMaterialProps = false;
    float materialRoughness = uRoughness;
    float materialMetallic = uMetallic;
    float materialAO = uAO;
    vec3 terrainNormalWS = normalize(Normal);

    vec4 baseColor;
    if (uTerrainPass) {
        vec3 Nw = normalize(Normal);
        float h = FragPos.y;
        float slope = 1.0 - abs(dot(Nw, vec3(0.0, 1.0, 0.0)));
        bool customMat = uTerrainMaterialEnabled;
        float cliffStart = customMat ? uTerrainCliffStart : 0.22;
        float cliffEnd = customMat ? uTerrainCliffEnd : 0.75;
        float snowStart = customMat ? uTerrainSnowStart : 8.0;
        float snowEnd = customMat ? uTerrainSnowEnd : 20.0;
        float lowStart = customMat ? uTerrainLowStart : -1.0;
        float lowEnd = customMat ? uTerrainLowEnd : 4.0;

        // Smooth biome blend from encoded biome channel (0..5).
        float biomeV = clamp(TexCoord.y * 5.0, 0.0, 5.0);
        float wOcean     = biomeWeight(biomeV, 0.0);
        float wPlains    = biomeWeight(biomeV, 1.0);
        float wForest    = biomeWeight(biomeV, 2.0);
        float wDesert    = biomeWeight(biomeV, 3.0);
        float wMountains = biomeWeight(biomeV, 4.0);
        float wTundra    = biomeWeight(biomeV, 5.0);
        float biomeSum = wOcean + wPlains + wForest + wDesert + wMountains + wTundra + 0.0001;
        wOcean /= biomeSum; wPlains /= biomeSum; wForest /= biomeSum;
        wDesert /= biomeSum; wMountains /= biomeSum; wTundra /= biomeSum;

        // Terrain masks used like splat-map channels.
        float cliffMask = smoothstep(cliffStart, cliffEnd, slope);
        float flatMask  = 1.0 - cliffMask;
        float highMask  = smoothstep(snowStart, snowEnd, h);
        float lowMask   = 1.0 - smoothstep(lowStart, lowEnd, h);

        float layerGrass = wPlains * 0.80 + wForest * 0.45 + wTundra * 0.10;
        float layerDirt  = wForest * 0.38 + wPlains * 0.20 + wDesert * 0.10 + lowMask * 0.25;
        float layerRock  = wMountains * 0.60 + wTundra * 0.28 + wDesert * 0.20 + cliffMask * 0.90;
        float layerSand  = wDesert * 0.72 + wOcean * 0.70 + lowMask * 0.22;
        float layerSnow  = wTundra * 0.82 + wMountains * highMask * 0.90;

        layerGrass *= flatMask;
        layerSand *= (0.65 + 0.35 * flatMask);
        layerSnow *= (0.45 + 0.55 * flatMask);

        float layerSum = layerGrass + layerDirt + layerRock + layerSand + layerSnow + 0.0001;
        layerGrass /= layerSum;
        layerDirt  /= layerSum;
        layerRock  /= layerSum;
        layerSand  /= layerSum;
        layerSnow  /= layerSum;

        // Triplanar procedural details (engine-native, no external terrain textures required).
        float macroScale = customMat ? uTerrainMacroScale : 0.05;
        float detailScale = customMat ? uTerrainDetailScale : 1.0;
        float macro = triplanarNoise(FragPos + vec3(17.3, 0.0, 9.1), Nw, macroScale);
        float gN = triplanarNoise(FragPos + vec3(11.0, 0.0, 23.0), Nw, detailScale * 0.45);
        float dN = triplanarNoise(FragPos + vec3(41.0, 0.0, 7.0),  Nw, detailScale * 0.75);
        float rN = triplanarNoise(FragPos + vec3(67.0, 0.0, 3.0),  Nw, detailScale * 1.05);
        float sN = triplanarNoise(FragPos + vec3(5.0, 0.0, 59.0),  Nw, detailScale * 0.40);
        float iN = triplanarNoise(FragPos + vec3(83.0, 0.0, 31.0), Nw, detailScale * 0.85);

        vec3 grassA = customMat ? uTerrainGrassA : vec3(0.17, 0.39, 0.12);
        vec3 grassB = customMat ? uTerrainGrassB : vec3(0.30, 0.56, 0.18);
        vec3 dirtA = customMat ? uTerrainDirtA : vec3(0.24, 0.18, 0.11);
        vec3 dirtB = customMat ? uTerrainDirtB : vec3(0.36, 0.26, 0.14);
        vec3 rockA = customMat ? uTerrainRockA : vec3(0.31, 0.31, 0.32);
        vec3 rockB = customMat ? uTerrainRockB : vec3(0.46, 0.43, 0.39);
        vec3 sandA = customMat ? uTerrainSandA : vec3(0.63, 0.55, 0.35);
        vec3 sandB = customMat ? uTerrainSandB : vec3(0.85, 0.76, 0.54);
        vec3 snowA = customMat ? uTerrainSnowA : vec3(0.78, 0.83, 0.90);
        vec3 snowB = customMat ? uTerrainSnowB : vec3(0.97, 0.98, 1.00);

        vec3 colGrass = mix(grassA, grassB, gN);
        colGrass = mix(colGrass, vec3(0.34, 0.41, 0.16), macro * 0.25);
        vec3 colDirt  = mix(dirtA, dirtB, dN);
        vec3 colRock  = mix(rockA, rockB, rN);
        vec3 colSand  = mix(sandA, sandB, sN);
        vec3 colSnow  = mix(snowA, snowB, iN * 0.65 + 0.35);

        vec3 terrainColor =
            colGrass * layerGrass +
            colDirt  * layerDirt  +
            colRock  * layerRock  +
            colSand  * layerSand  +
            colSnow  * layerSnow;

        if (customMat && uTerrainFlatGreenEnabled) {
            // Complete override: single flat color for poly-style world.
            terrainColor = uTerrainFlatGreenColor;
        }

        // Break up monotony and dampen saturation on steep surfaces.
        // Skip when flat green is enabled for a truly uniform poly look.
        if (!uTerrainFlatGreenEnabled) {
            float macroVarStrength = customMat ? uTerrainMacroVariationStrength : 0.20;
            float cliffDesat = customMat ? uTerrainCliffDesatStrength : 0.35;
            terrainColor *= (0.90 + macroVarStrength * macro);
            terrainColor = mix(terrainColor, terrainColor * vec3(0.90, 0.92, 0.95), cliffMask * cliffDesat);
        }
        terrainColor = clamp(terrainColor, vec3(0.0), vec3(1.0));

        if (uTerrainFlatGreenEnabled) {
            // Poly-style: flat roughness, flat AO, no detail normals.
            materialRoughness = 0.85;
            materialMetallic = 0.0;
            materialAO = 1.0;
            // Use raw geometry normal — no detail perturbation.
            terrainNormalWS = Nw;
        } else {
            // Terrain-specific PBR parameters.
            float roughGrass = customMat ? uTerrainRoughGrass : 0.84;
            float roughDirt = customMat ? uTerrainRoughDirt : 0.90;
            float roughRock = customMat ? uTerrainRoughRock : 0.63;
            float roughSand = customMat ? uTerrainRoughSand : 0.88;
            float roughSnow = customMat ? uTerrainRoughSnow : 0.42;
            materialRoughness =
                layerGrass * roughGrass +
                layerDirt  * roughDirt  +
                layerRock  * roughRock  +
                layerSand  * roughSand  +
                layerSnow  * roughSnow;
            materialRoughness = clamp(materialRoughness, 0.20, 0.98);

            materialMetallic = 0.0;
            materialAO = clamp(0.74 + flatMask * 0.16 - cliffMask * 0.07 + lowMask * 0.06, 0.55, 1.0);

            // Detail normal from triplanar noise derivatives.
            vec3 tangent = normalize(vec3(Nw.z, 0.0, -Nw.x));
            if (dot(tangent, tangent) < 0.0001) tangent = vec3(1.0, 0.0, 0.0);
            vec3 bitangent = normalize(cross(Nw, tangent));
            float eps = 0.45;
            float normalDetailScale = customMat ? uTerrainNormalDetailScale : 1.9;
            float normalStrength = customMat ? uTerrainNormalStrength : 0.85;
            float dT = triplanarNoise(FragPos + tangent * eps, Nw, normalDetailScale) -
                       triplanarNoise(FragPos - tangent * eps, Nw, normalDetailScale);
            float dB = triplanarNoise(FragPos + bitangent * eps, Nw, normalDetailScale) -
                       triplanarNoise(FragPos - bitangent * eps, Nw, normalDetailScale);
            terrainNormalWS = normalize(Nw - tangent * dT * normalStrength - bitangent * dB * normalStrength);
        }

        useTerrainMaterialProps = true;
        baseColor = vec4(terrainColor, 1.0);
    } else {
        baseColor = uUseColor ? uColor : texture(texDiffuse, TexCoord);
    }

    float alpha = baseColor.a;
    if (!useTerrainMaterialProps && uHasOpacityMap) {
        alpha *= sampleChannelValue(texture(texOpacity, TexCoord), uOpacityChannel);
    }
    alpha = clamp(alpha, 0.0, 1.0);
    if (!useTerrainMaterialProps && uAlphaCutoff > 0.0 && alpha < uAlphaCutoff) {
        discard;
    }
    baseColor.a = alpha;

    // ---------------- LIGHTING + SHADOWS ----------------
    vec3 N = useTerrainMaterialProps ? normalize(terrainNormalWS) : sampleNormalWS(Normal);
    vec3 V = normalize(uCameraPos - FragPos);

    vec3 albedo = toLinear(baseColor.rgb);
    
    // Read PBR textures or uniforms
    float roughness = materialRoughness;
    if (!useTerrainMaterialProps && uHasRoughnessMap) {
        roughness = sampleChannelValue(texture(texRoughness, TexCoord), uRoughnessChannel);
        if (uRoughnessMapIsGloss) {
            roughness = 1.0 - roughness;
        }
    }
    roughness = clamp(roughness, 0.04, 1.0);

    float metallic = materialMetallic;
    if (!useTerrainMaterialProps && uHasMetallicMap) {
        metallic = sampleChannelValue(texture(texMetallic, TexCoord), uMetallicChannel);
    }
    metallic = clamp(metallic, 0.0, 1.0);

    float ao = materialAO;
    if (!useTerrainMaterialProps && uHasAOMap) {
        ao = sampleChannelValue(texture(texAO, TexCoord), uAOChannel);
    }
    ao = clamp(ao, 0.0, 1.0);
    
    // F0 for dielectrics is mostly 0.04, for metals it's the albedo color
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    // Hemispheric Ambient (sky color vs ground color based on Normal Y)
    vec3 skyColor = vec3(0.6, 0.7, 0.8) * uAmbient;
    vec3 groundColor = vec3(0.2, 0.25, 0.2) * uAmbient;
    vec3 ambient = albedo * mix(groundColor, skyColor, N.y * 0.5 + 0.5) * (1.0 - metallic) + F0 * 0.1 * mix(groundColor, skyColor, N.y * 0.5 + 0.5);
    ambient *= ao;

    vec3 Lo = vec3(0.0);

    // ---------- SUN (Directional Light approximation) ----------
    vec3  Ls = normalize(-uLightDir); // Sun rays come from the light dir TO the fragment
    vec3  Hs = normalize(Ls + V);
    float NdotLs = max(dot(N, Ls), 0.0);

    float shadow = 0.0;
    if (NdotLs > 0.0) {
        vec4 fragPosLightSpace = uLightSpaceMatrix * vec4(FragPos, 1.0);
        shadow = ShadowDirectional(fragPosLightSpace);
        shadow = clamp(shadow, 0.0, 1.0);
    }

    vec3 sunRadiance = uSunColor * uSunIntensity;

    // Cook-Torrance BRDF for Sun
    float NDF = DistributionGGX(N, Hs, roughness);   
    float G   = GeometrySmith(N, V, Ls, roughness);      
    vec3 F    = fresnelSchlick(max(dot(Hs, V), 0.0), F0);

    vec3 numerator    = NDF * G * F; 
    float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, Ls), 0.0) + 0.0001;
    vec3 specular     = numerator / denominator;

    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;	

    Lo += (kD * albedo / PI + specular) * sunRadiance * NdotLs * (1.0 - shadow);

    // ---------- FIRE (non-shadowed point light) ----------
    if (uHasFire)
    {
        vec3  toFire = uFirePos - FragPos;
        float dist   = length(toFire);
        vec3  Lf     = (dist > 0.0001) ? (toFire / dist) : vec3(0.0, 1.0, 0.0);
        vec3  Hf = normalize(Lf + V);

        // Classic attenuation
        float attenuation = 1.0 / (uFireConstant + uFireLinear * dist + uFireQuadratic * (dist * dist));
        float flicker = 1.0 + uFireFlicker * sin(uTime * 17.0 + FragPos.x * 3.0 + FragPos.z * 2.0);
        vec3 fireRadiance = uFireColor * (uFireIntensity * attenuation * flicker);

        float NdotLf = max(dot(N, Lf), 0.0);

        // Cook-Torrance BRDF for Fire
        float NDF_fire = DistributionGGX(N, Hf, roughness);   
        float G_fire   = GeometrySmith(N, V, Lf, roughness);      
        vec3 F_fire    = fresnelSchlick(max(dot(Hf, V), 0.0), F0);

        vec3 num_fire    = NDF_fire * G_fire * F_fire; 
        float denom_fire = 4.0 * max(dot(N, V), 0.0) * max(dot(N, Lf), 0.0) + 0.0001;
        vec3 spec_fire   = num_fire / denom_fire;

        vec3 kS_fire = F_fire;
        vec3 kD_fire = vec3(1.0) - kS_fire;
        kD_fire *= 1.0 - metallic;	

        Lo += (kD_fire * albedo / PI + spec_fire) * fireRadiance * NdotLf;

        // Local ambient lift
        float amb = clamp(1.0 - dist / uFireAmbientRadius, 0.0, 1.0);
        amb = amb * amb;
        ambient += albedo * (uFireAmbient * amb) * uFireColor;
    }

    vec3 emissive = uEmissiveColor * uEmissiveStrength;
    if (!useTerrainMaterialProps && uHasEmissiveMap) {
        emissive += toLinear(texture(texEmissive, TexCoord).rgb) * uEmissiveStrength;
    }

    vec3 lit = ambient + Lo + emissive;

    // Tonemap + gamma
    lit = toneMapReinhard(lit);
    lit = toSRGB(lit);

    // --- FOG CALCULATION ---
    float dist = length(uCameraPos - FragPos);
    
    // Exponential fog formula: exp(-(distance * density)^2)
    float fogFactor = exp(-pow(dist * uFogDensity, 2.0));
    fogFactor = clamp(fogFactor, 0.0, 1.0);

    // Mix the scene color with the fog color based on distance
    lit = mix(uFogColor, lit, fogFactor); 

    FragColor = vec4(lit, baseColor.a);
}
