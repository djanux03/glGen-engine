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
uniform float uEmissiveBoost;
uniform float uEmissiveFlicker;
uniform float uAlphaCutoff;
uniform float uGamma;
uniform float uSceneExposure;

// Shadow uniforms (SUN as directional light with shadows)
uniform sampler2D shadowMap;        // texture unit 1
uniform sampler2DArray uShadowMapArray; // texture unit 16
uniform sampler2D uEnvMap;          // texture unit 2
uniform mat4  uLightSpaceMatrix;
uniform mat4  uLightSpaceMatrices[4];
uniform mat4  uViewMatrix;
uniform vec3  uLightDir;            // sun dir
uniform float uFarPlane;
uniform float uShadowStrength;
uniform bool  uShadowsEnabled;
uniform bool  uUseCascadedShadows;
uniform int   uCascadeCount;
uniform float uCascadeSplits[4];
uniform float uShadowNormalBias;
uniform float uShadowDepthBias;
uniform float uShadowSoftness;
uniform bool  uShowShadowCascades;
uniform vec3  uFogColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform bool  uAerialPerspectiveEnabled;
uniform float uAerialPerspectiveDensity;
uniform float uAerialPerspectiveStart;
uniform float uAerialPerspectiveHeightFalloff;
uniform float uAerialPerspectiveSkyBlend;
uniform float uAerialPerspectiveSunGlow;
uniform float uAerialPerspectiveDesaturation;
uniform vec3  uAerialHorizonColor;
uniform vec3  uAerialZenithColor;
uniform bool  uAmbientHemiEnabled;
uniform float uAmbientHemiIntensity;
uniform float uAmbientHorizonStrength;
uniform float uAmbientTerrainBoost;
uniform vec3  uAmbientSkyColor;
uniform vec3  uAmbientHorizonColor;
uniform vec3  uAmbientGroundColor;
uniform bool  uToonEnabled;
uniform int   uToonSteps;
uniform float uToonMin;
uniform bool  uShadowBandEnabled;
uniform int   uShadowBandSteps;
uniform float uShadowBandSoftness;
uniform bool  uAmbientRampEnabled;
uniform float uAmbientRampStrength;
uniform vec3  uAmbientRampTop;
uniform vec3  uAmbientRampBottom;
uniform bool  uRimEnabled;
uniform float uRimPower;
uniform float uRimStrength;
uniform vec3  uRimColor;
uniform bool  uEnvMapAvailable;
uniform mat3  uEnvRotation;
uniform float uEnvYaw;
uniform float uEnvIntensity;
uniform float uEnvDiffuseStrength;
uniform float uEnvSpecularStrength;

// NEW: Campfire point light (no shadows)
uniform bool  uHasFire;
uniform vec3  uFirePos;
uniform vec3  uFireDir;
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
uniform bool  uTerrainUseLayerTextures;
uniform bool  uTerrainHasGrassAlbedo;
uniform bool  uTerrainHasGrassNormal;
uniform bool  uTerrainHasGrassRoughness;
uniform bool  uTerrainHasDirtAlbedo;
uniform bool  uTerrainHasDirtNormal;
uniform bool  uTerrainHasDirtRoughness;
uniform float uTerrainLayerTextureTiling;
uniform float uTerrainLayerTextureStrength;
uniform float uTerrainLayerNormalStrength;
uniform float uTerrainLayerRoughnessStrength;
uniform bool  uTerrainUseGroundTextures;
uniform bool  uTerrainGroundFullOverride;
uniform bool  uTerrainHasGroundNormal;
uniform bool  uTerrainHasGroundRoughness;
uniform bool  uTerrainHasGroundHeight;
uniform float uTerrainGroundTiling;
uniform float uTerrainGroundBlendStrength;
uniform float uTerrainGroundRoughnessValue;
uniform float uTerrainGroundHeightStrength;
uniform bool  uTerrainGroundPseudoHeightEnabled;
uniform int   uTerrainGroundPseudoHeightSource;
uniform float uTerrainGroundPseudoHeightContrast;
uniform float uTerrainGroundPseudoHeightBias;
uniform bool  uTerrainGroundGradeEnabled;
uniform float uTerrainGroundGradeSaturation;
uniform float uTerrainGroundGradeContrast;
uniform float uTerrainGroundGradeGamma;
uniform vec3  uTerrainGroundGradeTint;
uniform float uTerrainGroundBrightness;
uniform float uTerrainGroundVariationStrength;
uniform float uTerrainGroundVariationScale;
uniform bool  uTerrainSunGlintEnabled;
uniform float uTerrainSunGlintIntensity;
uniform float uTerrainSunGlintSharpness;
uniform float uTerrainSunGlintMaskScale;
uniform float uTerrainSunGlintMaskStrength;
uniform float uTerrainSunGlintBaseSpecular;
uniform vec3  uTerrainSunGlintDirection;
uniform float uTerrainSunGlintBandWidth;
uniform sampler2D uTerrainGroundAlbedo;
uniform sampler2D uTerrainGroundRoughness;
uniform sampler2D uTerrainGroundNormal;
uniform sampler2D uTerrainGroundHeight;
uniform sampler2D uTerrainGrassAlbedo;
uniform sampler2D uTerrainDirtAlbedo;
uniform sampler2D uTerrainGrassNormal;
uniform sampler2D uTerrainGrassRoughness;
uniform sampler2D uTerrainDirtNormal;
uniform sampler2D uTerrainDirtRoughness;
uniform bool  uTerrainFlatGreenEnabled;
uniform vec3  uTerrainFlatGreenColor;
uniform int   uTerrainMaterialQuality;

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

float atmosphereFlow(vec3 worldPos, float dist) {
    vec2 wind = normalize(vec2(0.82, 0.57));
    vec2 p = worldPos.xz * 0.0018 + wind * uTime * 0.012;
    float broad = fbm(p);
    float bands = fbm(worldPos.xz * 0.006 + wind.yx * uTime * 0.020 +
                      vec2(worldPos.y * 0.0015, -worldPos.y * 0.0009));
    float distanceMask = smoothstep(35.0, 260.0, dist);
    return mix(1.0, mix(0.78, 1.24, broad) * mix(0.88, 1.16, bands),
               distanceMask);
}

int chooseShadowCascade(float viewDepth) {
    int cascadeIndex = max(uCascadeCount - 1, 0);
    for (int i = 0; i < 4; ++i) {
        if (i >= uCascadeCount) break;
        if (viewDepth <= uCascadeSplits[i]) {
            cascadeIndex = i;
            break;
        }
    }
    return clamp(cascadeIndex, 0, 3);
}

float sampleShadow2D(vec4 fragPosLightSpace, vec3 normal, float softnessScale) {
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    
    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) {
        return 0.0;
    }

    float currentDepth = projCoords.z;
    vec3 lightDir = normalize(-uLightDir);
    float slopeBias = max(0.0, 1.0 - dot(normal, lightDir));
    float bias = max(uShadowDepthBias + uShadowNormalBias * slopeBias,
                     uShadowDepthBias);
    
    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    texelSize *= max(0.25, uShadowSoftness * softnessScale);
    for(int x = -1; x <= 1; ++x) {
        for(int y = -1; y <= 1; ++y) {
            float pcfDepth =
                texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

float sampleShadowCascade(int cascadeIndex, vec3 normal, float softnessScale) {
    vec4 fragPosLightSpace =
        uLightSpaceMatrices[cascadeIndex] * vec4(FragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0) {
        return 0.0;
    }

    float currentDepth = projCoords.z;
    vec3 lightDir = normalize(-uLightDir);
    float slopeBias = max(0.0, 1.0 - dot(normal, lightDir));
    float bias = max(uShadowDepthBias + uShadowNormalBias * slopeBias,
                     uShadowDepthBias);
    bias *= 1.0 + float(cascadeIndex) * 0.18;

    ivec3 mapSize = textureSize(uShadowMapArray, 0);
    vec2 texelSize = 1.0 / vec2(max(mapSize.x, 1), max(mapSize.y, 1));
    texelSize *= max(0.25, uShadowSoftness * softnessScale *
                           (1.0 + float(cascadeIndex) * 0.35));

    float shadow = 0.0;
    for(int x = -1; x <= 1; ++x) {
        for(int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(
                uShadowMapArray,
                vec3(projCoords.xy + vec2(x, y) * texelSize,
                     float(cascadeIndex))).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

float ShadowDirectional(vec4 fragPosLightSpace) {
    if (!uShadowsEnabled || uShadowStrength <= 0.0) {
        return 0.0;
    }

    vec3 normal = normalize(Normal);
    if (!uUseCascadedShadows) {
        return sampleShadow2D(fragPosLightSpace, normal, 1.0) *
               uShadowStrength;
    }

    float viewDepth = abs((uViewMatrix * vec4(FragPos, 1.0)).z);
    int cascadeIndex = chooseShadowCascade(viewDepth);
    float shadow = sampleShadowCascade(cascadeIndex, normal, 1.0);

    if (cascadeIndex + 1 < uCascadeCount) {
        float prevSplit = cascadeIndex == 0 ? 0.0 : uCascadeSplits[cascadeIndex - 1];
        float split = uCascadeSplits[cascadeIndex];
        float blendRange = max((split - prevSplit) * 0.12, 2.0);
        float blendStart = split - blendRange;
        float blend = smoothstep(blendStart, split, viewDepth);
        float nextShadow = sampleShadowCascade(cascadeIndex + 1, normal, 1.0);
        shadow = mix(shadow, nextShadow, blend);
    }

    float lastSplit = uCascadeSplits[max(uCascadeCount - 1, 0)];
    float fade = 1.0 - smoothstep(lastSplit * 0.92, lastSplit, viewDepth);
    return shadow * fade * uShadowStrength;
}

// Gamma helpers
vec3 toLinear(vec3 srgb) { return pow(max(srgb, vec3(0.0)), vec3(uGamma)); }
vec3 toSRGB(vec3 lin)    { return pow(max(lin,  vec3(0.0)), vec3(1.0 / uGamma)); }

vec3 toneMapReinhard(vec3 c) { return c / (c + vec3(1.0)); }

vec3 applyAerialPerspective(vec3 lit) {
    float dist = length(uCameraPos - FragPos);
    float flow = atmosphereFlow(FragPos, dist);
    float heightDensity =
        uFogDensity * flow * exp(-max(FragPos.y, 0.0) * uFogHeightFalloff);
    float legacyFog = 1.0 - exp(-pow(dist * heightDensity, 2.0));
    legacyFog = clamp(legacyFog, 0.0, 1.0);

    if (!uAerialPerspectiveEnabled) {
        vec3 movingFogColor = uFogColor * mix(0.96, 1.05, flow);
        return mix(lit, movingFogColor, legacyFog);
    }

    vec3 viewDir = dist > 0.001 ? normalize(FragPos - uCameraPos)
                                : vec3(0.0, 0.0, -1.0);
    float rayDistance = max(dist - max(uAerialPerspectiveStart, 0.0), 0.0);
    float avgHeight = max((uCameraPos.y + FragPos.y) * 0.5, 0.0);
    float heightTerm =
        exp(-avgHeight * max(uAerialPerspectiveHeightFalloff, 0.0));
    float aerialFog = 1.0 - exp(-rayDistance *
                                max(uAerialPerspectiveDensity, 0.0) *
                                heightTerm * flow);
    aerialFog = clamp(aerialFog, 0.0, 1.0);

    float fogAmount = clamp(1.0 - (1.0 - legacyFog) * (1.0 - aerialFog),
                            0.0, 1.0);
    float horizonMask = pow(1.0 - clamp(abs(viewDir.y), 0.0, 1.0), 0.55);
    float zenithMix = smoothstep(-0.15, 0.85, viewDir.y) * 0.75;
    vec3 skyTint = mix(uAerialHorizonColor, uAerialZenithColor, zenithMix);
    skyTint = mix(uFogColor, skyTint, clamp(uAerialPerspectiveSkyBlend, 0.0, 1.0));
    skyTint *= mix(0.96, 1.06, flow);

    vec3 sunDir = normalize(-uLightDir);
    vec3 viewScatterDir = normalize(vec3(viewDir.x, viewDir.y * 0.35, viewDir.z));
    vec3 sunScatterDir = normalize(vec3(sunDir.x, sunDir.y * 0.35, sunDir.z));
    float sunForward = max(dot(viewScatterDir, sunScatterDir), 0.0);
    float sunScatter = pow(sunForward, 7.0) * horizonMask *
                       clamp(uAerialPerspectiveSunGlow, 0.0, 2.0) *
                       mix(0.85, 1.20, flow);
    vec3 atmosphereColor = skyTint + uSunColor * sunScatter;

    float luma = dot(lit, vec3(0.2126, 0.7152, 0.0722));
    vec3 transmitted =
        mix(lit, vec3(luma), fogAmount *
                                clamp(uAerialPerspectiveDesaturation, 0.0, 1.0));
    return mix(transmitted, atmosphereColor, fogAmount);
}

vec3 ambientHemisphere(vec3 normal, bool terrainSurface) {
    if (!uAmbientHemiEnabled) {
        return vec3(uAmbient);
    }

    float upT = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 hemi = mix(uAmbientGroundColor, uAmbientSkyColor, upT);
    float horizon = pow(1.0 - abs(clamp(normal.y, -1.0, 1.0)), 1.35);
    hemi = mix(hemi, uAmbientHorizonColor,
               clamp(horizon * uAmbientHorizonStrength, 0.0, 1.0));

    float terrainBoost = terrainSurface ? max(uAmbientTerrainBoost, 0.0) : 1.0;
    return hemi * uAmbient * max(uAmbientHemiIntensity, 0.0) * terrainBoost;
}

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

vec2 dirToEquirectUV(vec3 dir) {
    vec3 d = normalize(dir);
    float yaw = atan(d.z, d.x) * (0.5 / PI) + 0.5 + uEnvYaw;
    float pitch = acos(clamp(d.y, -1.0, 1.0)) / PI;
    return vec2(fract(yaw), clamp(pitch, 0.0, 1.0));
}

vec3 sampleEnvironment(vec3 dir, float lod) {
    if (!uEnvMapAvailable) return vec3(0.0);
    vec3 worldDir = normalize(uEnvRotation * dir);
    vec2 uv = dirToEquirectUV(worldDir);
    return textureLod(uEnvMap, uv, max(lod, 0.0)).rgb;
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

float triplanarFbm(vec3 worldPos, vec3 worldNormal, float scale) {
    float v = 0.0;
    float amp = 0.55;
    float freq = scale;
    for (int i = 0; i < 4; ++i) {
        v += triplanarNoise(worldPos, worldNormal, freq) * amp;
        freq *= 2.03;
        amp *= 0.48;
    }
    return clamp(v, 0.0, 1.0);
}

float triplanarRidgeNoise(vec3 worldPos, vec3 worldNormal, float scale) {
    float n = triplanarFbm(worldPos, worldNormal, scale);
    return clamp(1.0 - abs(n * 2.0 - 1.0), 0.0, 1.0);
}

vec3 terrainProceduralLayer(vec3 lowColor, vec3 highColor, vec3 accentColor,
                            float broad, float detail, float fleck,
                            float accentAmount) {
    vec3 c = mix(lowColor, highColor, smoothstep(0.18, 0.86, detail));
    float fineContrast = mix(0.82, 1.18, smoothstep(0.22, 0.82, fleck));
    c *= mix(1.0, fineContrast, 0.38);
    c = mix(c, accentColor, accentAmount * smoothstep(0.42, 0.92, broad));
    return clamp(c, vec3(0.0), vec3(1.0));
}

vec4 triplanarTexture(sampler2D tex, vec3 worldPos, vec3 worldNormal, float scale) {
    vec3 n = abs(normalize(worldNormal));
    vec3 w = pow(n, vec3(4.0));
    float sumW = w.x + w.y + w.z + 0.0001;
    w /= sumW;

    vec2 uvX = worldPos.yz * scale;
    vec2 uvY = worldPos.xz * scale;
    vec2 uvZ = worldPos.xy * scale;

    vec4 sx = texture(tex, uvX);
    vec4 sy = texture(tex, uvY);
    vec4 sz = texture(tex, uvZ);
    return sx * w.x + sy * w.y + sz * w.z;
}

vec2 rotateUV(vec2 uv, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return mat2(c, -s, s, c) * uv;
}

vec4 triplanarTextureVariation(sampler2D tex, vec3 worldPos, vec3 worldNormal,
                               float scale, float variationScale,
                               float variationStrength) {
    vec3 n = abs(normalize(worldNormal));
    vec3 w = pow(n, vec3(4.0));
    float sumW = w.x + w.y + w.z + 0.0001;
    w /= sumW;

    vec2 cell = floor(worldPos.xz * max(variationScale, 0.0001));
    float cellNoiseA = noise(cell + vec2(13.1, 7.7));
    float cellNoiseB = noise(cell + vec2(37.2, 19.4));
    float angle = floor(cellNoiseA * 4.0) * (PI * 0.5);
    vec2 offset = vec2(cellNoiseA - 0.5, cellNoiseB - 0.5) * 1.37;
    float blend = smoothstep(0.2, 0.8, noise(cell + vec2(71.3, 29.8))) *
                  clamp(variationStrength, 0.0, 1.0);

    vec2 uvX = worldPos.yz * scale;
    vec2 uvY = worldPos.xz * scale;
    vec2 uvZ = worldPos.xy * scale;

    vec4 sxA = texture(tex, uvX);
    vec4 syA = texture(tex, uvY);
    vec4 szA = texture(tex, uvZ);

    vec4 sxB = texture(tex, rotateUV(uvX + offset, angle));
    vec4 syB = texture(tex, rotateUV(uvY + offset, angle));
    vec4 szB = texture(tex, rotateUV(uvZ + offset, angle));

    vec4 sampleA = sxA * w.x + syA * w.y + szA * w.z;
    vec4 sampleB = sxB * w.x + syB * w.y + szB * w.z;
    return mix(sampleA, sampleB, blend);
}

vec3 unpackNormal(vec3 packedNormal) {
    return normalize(packedNormal * 2.0 - 1.0);
}

float pseudoHeightFromAlbedo(vec3 albedo) {
    return dot(albedo, vec3(0.2126, 0.7152, 0.0722));
}

float pseudoHeightFromNormal(vec3 packedNormal) {
    vec3 n = unpackNormal(packedNormal);
    return clamp(1.0 - n.z, 0.0, 1.0);
}

float shapePseudoHeight(float h) {
    h = clamp(h + uTerrainGroundPseudoHeightBias, 0.0, 1.0);
    h = pow(h, max(uTerrainGroundPseudoHeightContrast, 0.001));
    return clamp(h, 0.0, 1.0);
}

float sampleTerrainPseudoHeight(vec3 samplePos, vec3 worldNormal,
                                float groundTexScale, float groundVarScale,
                                float groundVarStrength) {
    vec4 sourceA = triplanarTextureVariation(
        uTerrainGroundAlbedo, samplePos, worldNormal, groundTexScale,
        groundVarScale, groundVarStrength);
    float h = pseudoHeightFromAlbedo(sourceA.rgb);
    if (uTerrainGroundPseudoHeightSource == 1 && uTerrainHasGroundNormal) {
        vec4 sourceN = triplanarTextureVariation(
            uTerrainGroundNormal, samplePos, worldNormal, groundTexScale,
            groundVarScale, groundVarStrength);
        h = pseudoHeightFromNormal(sourceN.xyz);
    }
    return shapePseudoHeight(h);
}

vec3 applyGroundGrade(vec3 c) {
    if (!uTerrainGroundGradeEnabled) return c;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, uTerrainGroundGradeSaturation);
    c = (c - 0.5) * uTerrainGroundGradeContrast + 0.5;
    c = pow(max(c, vec3(0.0)), vec3(1.0 / max(uTerrainGroundGradeGamma, 0.001)));
    c *= uTerrainGroundGradeTint;
    return clamp(c, vec3(0.0), vec3(1.0));
}

vec3 triplanarNormalWS(sampler2D tex, vec3 worldPos, vec3 worldNormal, float scale) {
    vec3 n = abs(normalize(worldNormal));
    vec3 w = pow(n, vec3(4.0));
    float sumW = w.x + w.y + w.z + 0.0001;
    w /= sumW;

    vec3 signN = sign(worldNormal);
    signN = mix(vec3(1.0), signN, step(vec3(0.0001), abs(signN)));

    vec3 sampleX = unpackNormal(texture(tex, worldPos.yz * scale).xyz);
    vec3 sampleY = unpackNormal(texture(tex, worldPos.xz * scale).xyz);
    vec3 sampleZ = unpackNormal(texture(tex, worldPos.xy * scale).xyz);

    vec3 worldX = normalize(vec3(sampleX.z * signN.x, sampleX.x, sampleX.y));
    vec3 worldY = normalize(vec3(sampleY.x, sampleY.z * signN.y, sampleY.y));
    vec3 worldZ = normalize(vec3(sampleZ.x, sampleZ.y, sampleZ.z * signN.z));

    return normalize(worldX * w.x + worldY * w.y + worldZ * w.z);
}

vec3 triplanarNormalWSVariation(sampler2D tex, vec3 worldPos, vec3 worldNormal,
                                float scale, float variationScale,
                                float variationStrength) {
    vec3 n = abs(normalize(worldNormal));
    vec3 w = pow(n, vec3(4.0));
    float sumW = w.x + w.y + w.z + 0.0001;
    w /= sumW;

    vec3 signN = sign(worldNormal);
    signN = mix(vec3(1.0), signN, step(vec3(0.0001), abs(signN)));

    vec2 cell = floor(worldPos.xz * max(variationScale, 0.0001));
    float cellNoiseA = noise(cell + vec2(13.1, 7.7));
    float cellNoiseB = noise(cell + vec2(37.2, 19.4));
    float angle = floor(cellNoiseA * 4.0) * (PI * 0.5);
    vec2 offset = vec2(cellNoiseA - 0.5, cellNoiseB - 0.5) * 1.37;
    float blend = smoothstep(0.2, 0.8, noise(cell + vec2(71.3, 29.8))) *
                  clamp(variationStrength, 0.0, 1.0);

    vec3 baseNormal = triplanarNormalWS(tex, worldPos, worldNormal, scale);

    vec3 sampleX = unpackNormal(texture(tex, rotateUV(worldPos.yz * scale + offset, angle)).xyz);
    vec3 sampleY = unpackNormal(texture(tex, rotateUV(worldPos.xz * scale + offset, angle)).xyz);
    vec3 sampleZ = unpackNormal(texture(tex, rotateUV(worldPos.xy * scale + offset, angle)).xyz);

    vec3 worldX = normalize(vec3(sampleX.z * signN.x, sampleX.x, sampleX.y));
    vec3 worldY = normalize(vec3(sampleY.x, sampleY.z * signN.y, sampleY.y));
    vec3 worldZ = normalize(vec3(sampleZ.x, sampleZ.y, sampleZ.z * signN.z));
    vec3 variedNormal = normalize(worldX * w.x + worldY * w.y + worldZ * w.z);

    return normalize(mix(baseNormal, variedNormal, blend));
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



        FragColor = vec4(cloudLit, finalAlpha);
        return;
    }

    // ---------------- BASE COLOR ----------------
    bool useTerrainMaterialProps = false;
    float materialRoughness = uRoughness;
    float materialMetallic = uMetallic;
    float materialAO = uAO;
    vec3 terrainNormalWS = normalize(Normal);
    float terrainSunGlintMask = 1.0;
    float terrainSpecularMul = 1.0;

    vec4 baseColor;
    if (uTerrainPass) {
        vec3 Nw = normalize(Normal);
        float h = FragPos.y;
        float slope = 1.0 - abs(dot(Nw, vec3(0.0, 1.0, 0.0)));
        vec3 terrainToCamera = uCameraPos - FragPos;
        float terrainDistanceSq = dot(terrainToCamera, terrainToCamera);
        int terrainQuality = clamp(uTerrainMaterialQuality, 0, 3);
        float terrainCheapDistance =
            terrainQuality >= 3 ? 220.0 :
            terrainQuality >= 2 ? 620.0 :
            terrainQuality >= 1 ? 1050.0 : 1550.0;
        float terrainTextureDistance =
            terrainQuality >= 3 ? 140.0 :
            terrainQuality >= 2 ? 420.0 :
            terrainQuality >= 1 ? 860.0 : 1350.0;
        float terrainDetailDistance =
            terrainQuality >= 3 ? 0.0 :
            terrainQuality >= 2 ? 320.0 :
            terrainQuality >= 1 ? 720.0 : 1180.0;
        float terrainNormalDistance =
            terrainQuality >= 3 ? 0.0 :
            terrainQuality >= 2 ? 280.0 :
            terrainQuality >= 1 ? 620.0 : 980.0;
        float terrainHeightDistance =
            terrainQuality >= 3 ? 0.0 :
            terrainQuality >= 2 ? 340.0 :
            terrainQuality >= 1 ? 760.0 : 1120.0;
        float terrainGlintDistance =
            terrainQuality >= 3 ? 0.0 :
            terrainQuality >= 2 ? 260.0 :
            terrainQuality >= 1 ? 560.0 : 920.0;
        bool terrainUseCheapMaterial =
            terrainDistanceSq > terrainCheapDistance * terrainCheapDistance;
        bool terrainSkipMaterialTextures =
            terrainUseCheapMaterial ||
            terrainDistanceSq > terrainTextureDistance * terrainTextureDistance;
        bool terrainSkipExpensiveDetails =
            terrainQuality >= 3 || terrainUseCheapMaterial ||
            terrainDistanceSq > terrainDetailDistance * terrainDetailDistance;
        bool terrainSkipNormalDetails =
            terrainQuality >= 3 || terrainUseCheapMaterial ||
            terrainDistanceSq > terrainNormalDistance * terrainNormalDistance;
        bool terrainSkipHeightDetails =
            terrainQuality >= 3 || terrainUseCheapMaterial ||
            terrainDistanceSq > terrainHeightDistance * terrainHeightDistance;
        bool terrainSkipGlint =
            terrainQuality >= 3 || terrainUseCheapMaterial ||
            terrainDistanceSq > terrainGlintDistance * terrainGlintDistance;
        bool customMat = uTerrainMaterialEnabled;
        float cliffStart = customMat ? uTerrainCliffStart : 0.22;
        float cliffEnd = customMat ? uTerrainCliffEnd : 0.75;
        float snowStart = customMat ? uTerrainSnowStart : 8.0;
        float snowEnd = customMat ? uTerrainSnowEnd : 20.0;
        float lowStart = customMat ? uTerrainLowStart : -1.0;
        float lowEnd = customMat ? uTerrainLowEnd : 4.0;
        float dirtStart = mix(cliffStart * 0.35, cliffStart * 0.65, 0.55);
        float dirtEnd = mix(cliffStart, cliffEnd, 0.42);

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

        float vegetationMaterial = TexCoord.x;
        if (vegetationMaterial > 1.5) {
            float grain = fbm(FragPos.xz * 2.6 + vec2(FragPos.y * 0.85, -FragPos.y * 0.45));
            float vertical = fbm(vec2(FragPos.y * 1.8, atan(Nw.z, Nw.x) * 2.0));
            bool pineLeaf = vegetationMaterial > 2.12 && vegetationMaterial < 2.25;
            bool broadLeaf = vegetationMaterial >= 2.25 && vegetationMaterial < 2.45;
            bool paleBark = vegetationMaterial >= 2.45 && vegetationMaterial < 2.58;
            bool deadWood = vegetationMaterial >= 2.58 && vegetationMaterial < 2.70;
            bool grassBlade = vegetationMaterial >= 2.70 && vegetationMaterial < 2.82;
            bool flowerPetal = vegetationMaterial >= 2.82;
            vec3 terrainColor = vec3(0.0);

            vec3 barkA = toLinear(vec3(0.20, 0.115, 0.060));
            vec3 barkB = toLinear(vec3(0.46, 0.285, 0.145));
            vec3 pineA = toLinear(vec3(0.035, 0.155, 0.060));
            vec3 pineB = toLinear(vec3(0.145, 0.360, 0.135));
            vec3 leafA = toLinear(vec3(0.075, 0.260, 0.080));
            vec3 leafB = toLinear(vec3(0.300, 0.520, 0.155));
            vec3 birchA = toLinear(vec3(0.70, 0.66, 0.55));
            vec3 birchB = toLinear(vec3(0.16, 0.13, 0.10));
            vec3 deadA = toLinear(vec3(0.135, 0.105, 0.075));
            vec3 deadB = toLinear(vec3(0.36, 0.30, 0.22));
            vec3 grassA = toLinear(vec3(0.090, 0.265, 0.075));
            vec3 grassB = toLinear(vec3(0.390, 0.640, 0.175));
            vec3 grassTip = toLinear(vec3(0.690, 0.760, 0.260));
            vec3 petalA = toLinear(vec3(0.940, 0.710, 0.285));
            vec3 petalB = toLinear(vec3(1.000, 0.900, 0.500));

            if (pineLeaf) {
                float needle = triplanarRidgeNoise(FragPos, Nw, 5.2);
                terrainColor = mix(pineA, pineB, smoothstep(0.20, 0.90, grain));
                terrainColor *= mix(0.76, 1.18, smoothstep(0.35, 0.92, needle));
                materialRoughness = 0.78;
                materialAO = 0.82;
            } else if (broadLeaf) {
                float fleck = triplanarRidgeNoise(FragPos + vec3(13.0, 0.0, 4.0), Nw, 4.1);
                terrainColor = mix(leafA, leafB, smoothstep(0.12, 0.86, grain));
                terrainColor *= mix(0.84, 1.22, smoothstep(0.28, 0.86, fleck));
                materialRoughness = 0.70;
                materialAO = 0.86;
            } else if (paleBark) {
                float stripes = smoothstep(0.58, 0.90, triplanarRidgeNoise(FragPos, Nw, 3.8));
                terrainColor = mix(birchA, birchB, stripes * 0.72 + vertical * 0.18);
                materialRoughness = 0.86;
                materialAO = 0.88;
            } else if (deadWood) {
                terrainColor = mix(deadA, deadB, smoothstep(0.20, 0.90, grain));
                terrainColor *= mix(0.72, 1.08, vertical);
                materialRoughness = 0.94;
                materialAO = 0.78;
            } else if (grassBlade) {
                float bladeFleck = triplanarRidgeNoise(FragPos + vec3(3.0, 0.0, 19.0), Nw, 7.5);
                float heightTint = smoothstep(0.12, 1.05, FragPos.y - floor(FragPos.y));
                float windTint = fbm(FragPos.xz * 1.8 + vec2(uTime * 0.18, -uTime * 0.11));
                terrainColor = mix(grassA, grassB, smoothstep(0.12, 0.88, grain));
                terrainColor = mix(terrainColor, grassTip, heightTint * 0.28);
                terrainColor *= mix(0.82, 1.22, smoothstep(0.20, 0.88, bladeFleck) * 0.7 + windTint * 0.3);
                materialRoughness = 0.74;
                materialAO = 0.92;
            } else if (flowerPetal) {
                float petalPulse = fbm(FragPos.xz * 8.0 + vec2(uTime * 0.06));
                terrainColor = mix(petalA, petalB, smoothstep(0.18, 0.82, grain + petalPulse * 0.25));
                materialRoughness = 0.66;
                materialAO = 0.95;
            } else {
                float grooves = smoothstep(0.38, 0.92, triplanarRidgeNoise(FragPos, Nw, 3.2));
                terrainColor = mix(barkA, barkB, smoothstep(0.18, 0.88, grain));
                terrainColor *= mix(0.70, 1.12, grooves * 0.7 + vertical * 0.3);
                materialRoughness = 0.90;
                materialAO = 0.80;
            }
            materialMetallic = 0.0;
            terrainNormalWS = normalize(mix(Nw, normalize(Nw + vec3((grain - 0.5) * 0.18, 0.0,
                                                                     (vertical - 0.5) * 0.18)),
                                            0.45));
            useTerrainMaterialProps = true;
            baseColor = vec4(clamp(terrainColor, vec3(0.0), vec3(1.0)), 1.0);
        } else if (uTerrainFlatGreenEnabled) {
            // Fast stylized path: no triplanar noise, no detail normal synthesis,
            // no terrain PBR layer blending. Keep only broad height/slope tinting
            // so the terrain still reads as shaped instead of fully flat.
            float dirtMask = smoothstep(dirtStart, dirtEnd, slope);
            float cliffMask = smoothstep(cliffStart, cliffEnd, slope);
            dirtMask *= (1.0 - cliffMask);
            float lowMask = 1.0 - smoothstep(lowStart, lowEnd, h);
            float highMask = smoothstep(snowStart, snowEnd, h);

            vec3 baseGreen = uTerrainFlatGreenColor;
            vec3 lowTint = baseGreen * vec3(0.78, 0.86, 0.78);
            vec3 dirtTint = mix(toLinear(customMat ? uTerrainDirtA : vec3(0.24, 0.18, 0.11)),
                                toLinear(customMat ? uTerrainDirtB : vec3(0.36, 0.26, 0.14)),
                                0.45);
            vec3 rockTint = mix(toLinear(customMat ? uTerrainRockA : vec3(0.31, 0.31, 0.32)),
                                toLinear(customMat ? uTerrainRockB : vec3(0.46, 0.43, 0.39)),
                                0.40);
            vec3 highTint = mix(baseGreen, vec3(0.62, 0.70, 0.63), 0.35);

            vec3 terrainColor = baseGreen;
            terrainColor = mix(terrainColor, lowTint, lowMask * 0.35);
            terrainColor = mix(terrainColor, dirtTint, dirtMask * 0.75);
            terrainColor = mix(terrainColor, rockTint, cliffMask * 0.82);
            terrainColor = mix(terrainColor, highTint, highMask * 0.20);
            terrainColor = clamp(terrainColor, vec3(0.0), vec3(1.0));

            materialRoughness = 0.92;
            materialMetallic = 0.0;
            materialAO = clamp(0.94 - dirtMask * 0.04 - cliffMask * 0.10 + lowMask * 0.04, 0.72, 1.0);
            terrainNormalWS = Nw;
            useTerrainMaterialProps = true;
            baseColor = vec4(terrainColor, 1.0);
        } else {

        // Terrain masks used like splat-map channels.
        float cliffMask = smoothstep(cliffStart, cliffEnd, slope);
        float dirtSlopeMask = smoothstep(dirtStart, dirtEnd, slope) * (1.0 - cliffMask);
        float flatMask  = 1.0 - cliffMask;
        float highMask  = smoothstep(snowStart, snowEnd, h);
        float lowMask   = 1.0 - smoothstep(lowStart, lowEnd, h);
        float greenBiomeMask = clamp(wPlains + wForest + wTundra * 0.35, 0.0, 1.0);

        // Triplanar procedural details (engine-native, no external terrain textures required).
        float macroScale = customMat ? uTerrainMacroScale : 0.05;
        float detailScale = customMat ? uTerrainDetailScale : 1.0;
        float erosionNoise = terrainSkipExpensiveDetails ? 0.5 :
            fbm(FragPos.xz * 0.030 + vec2(h * 0.017, -h * 0.011));
        float slopeStreak = terrainSkipExpensiveDetails ? 0.5 :
            triplanarRidgeNoise(FragPos + vec3(0.0, h * 0.55, 0.0), Nw, detailScale * 0.32);
        float layerGrass = wPlains * 0.92 + wForest * 0.70 + wTundra * 0.16;
        float layerDirt  = wForest * 0.28 + wPlains * 0.16 + wDesert * 0.10 + lowMask * 0.22;
        float layerRock  = wMountains * 0.60 + wTundra * 0.28 + wDesert * 0.20;
        float layerSand  = wDesert * 0.72 + wOcean * 0.70 + lowMask * 0.22;
        float layerSnow  = wTundra * 0.82 + wMountains * highMask * 0.90;

        layerGrass *= max(0.0, 1.0 - dirtSlopeMask * 0.95 - cliffMask * 1.15);
        layerGrass *= mix(0.82, 1.10, erosionNoise);
        layerDirt += dirtSlopeMask * (0.88 + 0.28 * slopeStreak) *
                     (0.95 * greenBiomeMask + 0.20 * wMountains);
        layerDirt += smoothstep(0.38, 0.78, slopeStreak) * 0.12 * flatMask *
                     (1.0 - highMask);
        layerRock += cliffMask * (1.08 + 0.36 * slopeStreak) *
                     (1.15 * greenBiomeMask + 0.90) +
                     dirtSlopeMask * 0.18 * wMountains;
        layerSand *= (0.65 + 0.35 * flatMask);
        layerSnow *= (0.45 + 0.55 * flatMask);

        float layerSum = layerGrass + layerDirt + layerRock + layerSand + layerSnow + 0.0001;
        layerGrass /= layerSum;
        layerDirt  /= layerSum;
        layerRock  /= layerSum;
        layerSand  /= layerSum;
        layerSnow  /= layerSum;

        float macro = terrainSkipExpensiveDetails ? 0.5 :
            triplanarFbm(FragPos + vec3(17.3, 0.0, 9.1), Nw, macroScale);
        float gN = terrainSkipExpensiveDetails ? 0.5 :
            triplanarFbm(FragPos + vec3(11.0, 0.0, 23.0), Nw, detailScale * 0.42);
        float dN = terrainSkipExpensiveDetails ? 0.5 :
            triplanarFbm(FragPos + vec3(41.0, 0.0, 7.0),  Nw, detailScale * 0.68);
        float rN = terrainSkipExpensiveDetails ? 0.5 :
            triplanarFbm(FragPos + vec3(67.0, 0.0, 3.0),  Nw, detailScale * 0.95);
        float sN = terrainSkipExpensiveDetails ? 0.5 :
            triplanarFbm(FragPos + vec3(5.0, 0.0, 59.0),  Nw, detailScale * 0.38);
        float iN = terrainSkipExpensiveDetails ? 0.5 :
            triplanarFbm(FragPos + vec3(83.0, 0.0, 31.0), Nw, detailScale * 0.78);
        float finePebble = terrainSkipExpensiveDetails ? 0.5 :
            triplanarRidgeNoise(FragPos + vec3(3.4, 0.0, 12.7), Nw, detailScale * 3.15);
        float grassBlade = terrainSkipExpensiveDetails ? 0.5 :
            triplanarRidgeNoise(FragPos + vec3(21.6, 0.0, 4.2), Nw, detailScale * 2.25);
        float rockStrata = terrainSkipExpensiveDetails ? 0.5 :
            triplanarRidgeNoise(FragPos + vec3(h * 0.45, 0.0, h * 0.18), Nw, detailScale * 1.55);

        vec3 grassA = toLinear(customMat ? uTerrainGrassA : vec3(0.17, 0.39, 0.12));
        vec3 grassB = toLinear(customMat ? uTerrainGrassB : vec3(0.30, 0.56, 0.18));
        vec3 dirtA = toLinear(customMat ? uTerrainDirtA : vec3(0.24, 0.18, 0.11));
        vec3 dirtB = toLinear(customMat ? uTerrainDirtB : vec3(0.36, 0.26, 0.14));
        vec3 rockA = toLinear(customMat ? uTerrainRockA : vec3(0.31, 0.31, 0.32));
        vec3 rockB = toLinear(customMat ? uTerrainRockB : vec3(0.46, 0.43, 0.39));
        vec3 sandA = toLinear(customMat ? uTerrainSandA : vec3(0.63, 0.55, 0.35));
        vec3 sandB = toLinear(customMat ? uTerrainSandB : vec3(0.85, 0.76, 0.54));
        vec3 snowA = toLinear(customMat ? uTerrainSnowA : vec3(0.78, 0.83, 0.90));
        vec3 snowB = toLinear(customMat ? uTerrainSnowB : vec3(0.97, 0.98, 1.00));

        vec3 colGrass = terrainProceduralLayer(
            grassA, grassB, toLinear(vec3(0.22, 0.30, 0.09)),
            macro, gN, grassBlade, 0.24);
        colGrass = mix(colGrass, colGrass * toLinear(vec3(0.72, 0.82, 0.62)),
                       smoothstep(0.48, 0.90, slopeStreak) * 0.18);
        vec3 colDirt = terrainProceduralLayer(
            dirtA, dirtB, toLinear(vec3(0.16, 0.105, 0.065)),
            erosionNoise, dN, finePebble, 0.34);
        vec3 colRock = terrainProceduralLayer(
            rockA, rockB, toLinear(vec3(0.20, 0.20, 0.21)),
            rockStrata, rN, finePebble, 0.42);
        colRock = mix(colRock, colRock * toLinear(vec3(1.18, 1.13, 1.02)),
                      smoothstep(0.68, 0.96, rockStrata) * 0.32);
        vec3 colSand = terrainProceduralLayer(
            sandA, sandB, toLinear(vec3(0.48, 0.40, 0.24)),
            macro, sN, finePebble, 0.20);
        vec3 colSnow = terrainProceduralLayer(
            snowA, snowB, toLinear(vec3(0.70, 0.78, 0.88)),
            iN, triplanarRidgeNoise(FragPos + vec3(6.0, 0.0, 13.0), Nw,
                                    detailScale * 0.95),
            finePebble, 0.18);

        if (uTerrainUseLayerTextures && !terrainSkipMaterialTextures) {
            float layerTexTiling = max(uTerrainLayerTextureTiling, 0.0001);
            float layerTexStrength = clamp(uTerrainLayerTextureStrength, 0.0, 1.0);
            if (uTerrainHasGrassAlbedo) {
                vec3 grassTex = triplanarTextureVariation(
                    uTerrainGrassAlbedo, FragPos, Nw, layerTexTiling,
                    max(uTerrainGroundVariationScale, 0.0001),
                    clamp(uTerrainGroundVariationStrength, 0.0, 1.0)).rgb;
                colGrass = mix(colGrass, grassTex, layerTexStrength);
            }
            if (uTerrainHasDirtAlbedo) {
                vec3 dirtTex = triplanarTextureVariation(
                    uTerrainDirtAlbedo, FragPos, Nw, layerTexTiling,
                    max(uTerrainGroundVariationScale, 0.0001),
                    clamp(uTerrainGroundVariationStrength, 0.0, 1.0)).rgb;
                colDirt = mix(colDirt, dirtTex, layerTexStrength);
            }
        }

        vec3 terrainColor =
            colGrass * layerGrass +
            colDirt  * layerDirt  +
            colRock  * layerRock  +
            colSand  * layerSand  +
            colSnow  * layerSnow;

        if (uTerrainUseGroundTextures && !uTerrainUseLayerTextures &&
            !terrainSkipMaterialTextures) {
            float groundVarStrength = clamp(uTerrainGroundVariationStrength, 0.0, 1.0);
            float groundVarScale = max(uTerrainGroundVariationScale, 0.0001);
            vec3 groundAlbedo =
                triplanarTextureVariation(uTerrainGroundAlbedo, FragPos, Nw,
                                          max(uTerrainGroundTiling, 0.0001),
                                          groundVarScale, groundVarStrength).rgb;
            groundAlbedo = applyGroundGrade(groundAlbedo);
            groundAlbedo *= max(uTerrainGroundBrightness, 0.0);
            float groundWeight = uTerrainGroundFullOverride
                                     ? 1.0
                                     : clamp((layerGrass + layerDirt) *
                                                 max(uTerrainGroundBlendStrength, 0.0),
                                             0.0, 1.0);
            terrainColor = mix(terrainColor, groundAlbedo, groundWeight);
        }

        float macroVarStrength = customMat ? uTerrainMacroVariationStrength : 0.20;
        float cliffDesat = customMat ? uTerrainCliffDesatStrength : 0.35;
        if (!uTerrainGroundFullOverride || uTerrainUseLayerTextures) {
            terrainColor *= (0.90 + macroVarStrength * macro);
            terrainColor = mix(terrainColor, terrainColor * vec3(0.90, 0.92, 0.95), cliffMask * cliffDesat);
        }
        terrainColor = clamp(terrainColor, vec3(0.0), vec3(1.0));

        // Terrain-specific PBR parameters.
        float roughGrass = customMat ? uTerrainRoughGrass : 0.84;
        float roughDirt = customMat ? uTerrainRoughDirt : 0.90;
        float roughRock = customMat ? uTerrainRoughRock : 0.63;
        float roughSand = customMat ? uTerrainRoughSand : 0.88;
        float roughSnow = customMat ? uTerrainRoughSnow : 0.42;
        if (uTerrainUseLayerTextures && !terrainSkipMaterialTextures) {
            float layerTexTiling = max(uTerrainLayerTextureTiling, 0.0001);
            float roughStrength = clamp(uTerrainLayerRoughnessStrength, 0.0, 1.0);
            float variationScale = max(uTerrainGroundVariationScale, 0.0001);
            float variationStrength = clamp(uTerrainGroundVariationStrength, 0.0, 1.0);
            if (uTerrainHasGrassRoughness) {
                float sampledGrassRoughness = triplanarTextureVariation(
                    uTerrainGrassRoughness, FragPos, Nw, layerTexTiling,
                    variationScale, variationStrength).r;
                roughGrass = mix(roughGrass, sampledGrassRoughness, roughStrength);
            }
            if (uTerrainHasDirtRoughness) {
                float sampledDirtRoughness = triplanarTextureVariation(
                    uTerrainDirtRoughness, FragPos, Nw, layerTexTiling,
                    variationScale, variationStrength).r;
                roughDirt = mix(roughDirt, sampledDirtRoughness, roughStrength);
            }
        }
        materialRoughness =
            layerGrass * roughGrass +
            layerDirt  * roughDirt  +
            layerRock  * roughRock  +
            layerSand  * roughSand  +
            layerSnow  * roughSnow;
        if (!terrainSkipExpensiveDetails && !uTerrainUseGroundTextures) {
            float roughBreakup =
                (finePebble - 0.5) * 0.10 * (layerDirt + layerRock + layerSand) +
                (grassBlade - 0.5) * 0.05 * layerGrass -
                smoothstep(0.62, 0.95, rockStrata) * 0.10 * layerRock;
            materialRoughness += roughBreakup;
        }
        if (uTerrainUseGroundTextures && !uTerrainUseLayerTextures &&
            !terrainSkipMaterialTextures) {
            float groundWeight = uTerrainGroundFullOverride
                                     ? 1.0
                                     : clamp((layerGrass + layerDirt) *
                                                 max(uTerrainGroundBlendStrength, 0.0),
                                             0.0, 1.0);
            float sampledGroundRoughness = uTerrainGroundRoughnessValue;
            if (uTerrainHasGroundRoughness) {
                vec4 roughTex = triplanarTextureVariation(
                    uTerrainGroundRoughness, FragPos, Nw,
                    max(uTerrainGroundTiling, 0.0001),
                    max(uTerrainGroundVariationScale, 0.0001),
                    clamp(uTerrainGroundVariationStrength, 0.0, 1.0));
                sampledGroundRoughness = roughTex.r;
            }
            materialRoughness =
                mix(materialRoughness, sampledGroundRoughness, groundWeight);
        }
        materialRoughness = clamp(materialRoughness, 0.20, 0.98);

        materialMetallic = 0.0;
        materialAO = (uTerrainGroundFullOverride && !uTerrainUseLayerTextures)
                         ? 1.0
                         : clamp(0.74 + flatMask * 0.16 - cliffMask * 0.07 + lowMask * 0.06, 0.55, 1.0);

        // Detail normal from triplanar noise derivatives.
        vec3 tangent = normalize(vec3(Nw.z, 0.0, -Nw.x));
        if (dot(tangent, tangent) < 0.0001) tangent = vec3(1.0, 0.0, 0.0);
        vec3 bitangent = normalize(cross(Nw, tangent));
        float eps = 0.45;
        float normalDetailScale = customMat ? uTerrainNormalDetailScale : 1.9;
        float normalStrength = customMat ? uTerrainNormalStrength : 0.85;
        if (terrainSkipNormalDetails) {
            terrainNormalWS = normalize(Nw);
        } else if (uTerrainGroundFullOverride && !uTerrainUseLayerTextures) {
            terrainNormalWS = normalize(Nw);
        } else {
            float lowDetailT =
                triplanarFbm(FragPos + tangent * eps, Nw, normalDetailScale) -
                triplanarFbm(FragPos - tangent * eps, Nw, normalDetailScale);
            float lowDetailB =
                triplanarFbm(FragPos + bitangent * eps, Nw, normalDetailScale) -
                triplanarFbm(FragPos - bitangent * eps, Nw, normalDetailScale);
            float microScale = normalDetailScale * 2.65;
            float microEps = eps * 0.42;
            float microT =
                triplanarRidgeNoise(FragPos + tangent * microEps, Nw, microScale) -
                triplanarRidgeNoise(FragPos - tangent * microEps, Nw, microScale);
            float microB =
                triplanarRidgeNoise(FragPos + bitangent * microEps, Nw, microScale) -
                triplanarRidgeNoise(FragPos - bitangent * microEps, Nw, microScale);
            float materialNormalWeight =
                layerGrass * 0.45 + layerDirt * 0.68 + layerRock * 1.15 +
                layerSand * 0.50 + layerSnow * 0.25;
            float dT = lowDetailT + microT * 0.34 * materialNormalWeight;
            float dB = lowDetailB + microB * 0.34 * materialNormalWeight;
            terrainNormalWS =
                normalize(Nw - tangent * dT * normalStrength -
                          bitangent * dB * normalStrength);
        }

        if (!terrainSkipNormalDetails && uTerrainUseLayerTextures &&
            (uTerrainHasGrassNormal || uTerrainHasDirtNormal)) {
            float layerTexTiling = max(uTerrainLayerTextureTiling, 0.0001);
            float variationScale = max(uTerrainGroundVariationScale, 0.0001);
            float variationStrength = clamp(uTerrainGroundVariationStrength, 0.0, 1.0);
            vec3 layerNormalWS = vec3(0.0);
            float layerNormalWeight = 0.0;
            if (uTerrainHasGrassNormal) {
                vec3 grassNormalWS = triplanarNormalWS(
                    uTerrainGrassNormal, FragPos, Nw, layerTexTiling);
                if (uTerrainGroundVariationStrength > 0.001) {
                    grassNormalWS = triplanarNormalWSVariation(
                        uTerrainGrassNormal, FragPos, Nw, layerTexTiling,
                        variationScale, variationStrength);
                }
                layerNormalWS += grassNormalWS * layerGrass;
                layerNormalWeight += layerGrass;
            }
            if (uTerrainHasDirtNormal) {
                vec3 dirtNormalWS = triplanarNormalWS(
                    uTerrainDirtNormal, FragPos, Nw, layerTexTiling);
                if (uTerrainGroundVariationStrength > 0.001) {
                    dirtNormalWS = triplanarNormalWSVariation(
                        uTerrainDirtNormal, FragPos, Nw, layerTexTiling,
                        variationScale, variationStrength);
                }
                layerNormalWS += dirtNormalWS * layerDirt;
                layerNormalWeight += layerDirt;
            }
            if (layerNormalWeight > 0.0001) {
                layerNormalWS = normalize(layerNormalWS / layerNormalWeight);
                float normalBlend = clamp(layerNormalWeight *
                                              max(uTerrainLayerNormalStrength, 0.0),
                                          0.0, 1.0);
                terrainNormalWS = normalize(mix(terrainNormalWS, layerNormalWS,
                                                normalBlend));
            }
        }

        if (!terrainSkipNormalDetails &&
            uTerrainUseGroundTextures && !uTerrainUseLayerTextures &&
            uTerrainHasGroundNormal) {
            float groundWeight = uTerrainGroundFullOverride
                                     ? 1.0
                                     : clamp((layerGrass + layerDirt) *
                                                 max(uTerrainGroundBlendStrength, 0.0),
                                             0.0, 1.0);
            vec3 groundNormalWS = triplanarNormalWS(
                uTerrainGroundNormal, FragPos, Nw, max(uTerrainGroundTiling, 0.0001));
            if (uTerrainGroundVariationStrength > 0.001) {
                groundNormalWS = triplanarNormalWSVariation(
                    uTerrainGroundNormal, FragPos, Nw,
                    max(uTerrainGroundTiling, 0.0001),
                    max(uTerrainGroundVariationScale, 0.0001),
                    clamp(uTerrainGroundVariationStrength, 0.0, 1.0));
            }
            terrainNormalWS = normalize(mix(terrainNormalWS, groundNormalWS, groundWeight));
        }

        if (!terrainSkipHeightDetails &&
            uTerrainUseGroundTextures && !uTerrainUseLayerTextures &&
            uTerrainHasGroundHeight &&
            uTerrainGroundHeightStrength > 0.001) {
            float groundWeight = uTerrainGroundFullOverride
                                     ? 1.0
                                     : clamp((layerGrass + layerDirt) *
                                                 max(uTerrainGroundBlendStrength, 0.0),
                                             0.0, 1.0);
            float groundVarStrength = clamp(uTerrainGroundVariationStrength, 0.0, 1.0);
            float groundVarScale = max(uTerrainGroundVariationScale, 0.0001);
            float groundTexScale = max(uTerrainGroundTiling, 0.0001);
            float heightEps = 0.18 / groundTexScale;
            float hCenter = triplanarTextureVariation(
                uTerrainGroundHeight, FragPos, Nw, groundTexScale,
                groundVarScale, groundVarStrength).r;
            float hT = triplanarTextureVariation(
                uTerrainGroundHeight, FragPos + tangent * heightEps, Nw, groundTexScale,
                groundVarScale, groundVarStrength).r;
            float hB = triplanarTextureVariation(
                uTerrainGroundHeight, FragPos + bitangent * heightEps, Nw, groundTexScale,
                groundVarScale, groundVarStrength).r;
            float dHT = (hT - hCenter) * uTerrainGroundHeightStrength;
            float dHB = (hB - hCenter) * uTerrainGroundHeightStrength;
            vec3 heightNormalWS =
                normalize(terrainNormalWS - tangent * dHT - bitangent * dHB);
            terrainNormalWS =
                normalize(mix(terrainNormalWS, heightNormalWS, groundWeight));
        } else if (!terrainSkipHeightDetails &&
                   uTerrainUseGroundTextures && !uTerrainUseLayerTextures &&
                   uTerrainGroundPseudoHeightEnabled &&
                   uTerrainGroundHeightStrength > 0.001) {
            float groundWeight = uTerrainGroundFullOverride
                                     ? 1.0
                                     : clamp((layerGrass + layerDirt) *
                                                 max(uTerrainGroundBlendStrength, 0.0),
                                             0.0, 1.0);
            float groundVarStrength = clamp(uTerrainGroundVariationStrength, 0.0, 1.0);
            float groundVarScale = max(uTerrainGroundVariationScale, 0.0001);
            float groundTexScale = max(uTerrainGroundTiling, 0.0001);
            float heightEps = 0.18 / groundTexScale;

            float hCenter = sampleTerrainPseudoHeight(
                FragPos, Nw, groundTexScale, groundVarScale, groundVarStrength);
            float hT = sampleTerrainPseudoHeight(
                FragPos + tangent * heightEps, Nw, groundTexScale,
                groundVarScale, groundVarStrength);
            float hB = sampleTerrainPseudoHeight(
                FragPos + bitangent * heightEps, Nw, groundTexScale,
                groundVarScale, groundVarStrength);
            float dHT = (hT - hCenter) * uTerrainGroundHeightStrength;
            float dHB = (hB - hCenter) * uTerrainGroundHeightStrength;
            vec3 heightNormalWS =
                normalize(terrainNormalWS - tangent * dHT - bitangent * dHB);
            terrainNormalWS =
                normalize(mix(terrainNormalWS, heightNormalWS, groundWeight));
        }

        if (terrainSkipGlint) {
            terrainSunGlintMask = 0.0;
        } else if (uTerrainSunGlintEnabled) {
            float glintMaskScale = max(uTerrainSunGlintMaskScale, 0.0001);
            vec2 glintUv = FragPos.xz * glintMaskScale;
            float glintField =
                fbm(glintUv + vec2(17.2, -9.4)) * 0.72 +
                fbm(glintUv * 0.47 + vec2(-31.0, 22.0)) * 0.28;
            float glintPatch = smoothstep(0.62, 0.90, glintField);
            float glintLayer = clamp(cliffMask * 0.45 + layerDirt * 0.65 +
                                         layerGrass * 0.38 + layerRock * 0.34 +
                                         layerSand * 0.22 + layerSnow * 0.12,
                                     0.0, 1.0);
            terrainSunGlintMask =
                clamp(mix(1.0, glintPatch,
                          clamp(uTerrainSunGlintMaskStrength, 0.0, 1.0)) *
                          glintLayer,
                      0.0, 1.0);
            terrainSunGlintMask =
                smoothstep(0.03, 0.55, terrainSunGlintMask);
            terrainSpecularMul = clamp(uTerrainSunGlintBaseSpecular, 0.0, 1.0);
        }

        useTerrainMaterialProps = true;
        baseColor = vec4(terrainColor, 1.0);
        }
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

    vec3 albedo = baseColor.rgb;
    if (!useTerrainMaterialProps && uUseColor) {
        albedo = toLinear(baseColor.rgb);
    }
    
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

    vec3 ambientColor = ambientHemisphere(N, useTerrainMaterialProps);
    vec3 ambient = albedo * ambientColor * (1.0 - metallic) +
                   F0 * 0.08 * ambientColor;
    if (uAmbientRampEnabled) {
        float rampT = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 ramp = mix(uAmbientRampBottom, uAmbientRampTop, rampT);
        ambient += albedo * ramp * uAmbientRampStrength;
    }
    ambient *= ao;

    if (uEnvMapAvailable) {
        vec3 envDiffuse = sampleEnvironment(N, 5.0);
        vec3 R = reflect(-V, N);
        float envLod = mix(0.0, 6.0, roughness * roughness);
        vec3 envSpecular = sampleEnvironment(R, envLod);
        float NdotV = max(dot(N, V), 0.0);
        vec3 envF = fresnelSchlick(NdotV, F0);
        vec3 envKD = (vec3(1.0) - envF) * (1.0 - metallic);
        ambient += envDiffuse * albedo * envKD *
                   (uEnvIntensity * uEnvDiffuseStrength * ao);
        ambient += envSpecular * envF *
                   (uEnvIntensity * uEnvSpecularStrength * mix(1.0, ao, 0.35) *
                    terrainSpecularMul);
    }

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
    float terrainSunGlint = 0.0;

    if (useTerrainMaterialProps && uTerrainSunGlintEnabled) {
        vec3 Lg = normalize(uTerrainSunGlintDirection);
        vec3 Hg = normalize(Lg + V);
        float ng = max(dot(N, Lg), 0.0);
        float nh = max(dot(N, Hg), 0.0);
        float nv = max(dot(N, V), 0.0);
        vec3 reflectedGlint = normalize(reflect(-Lg, N));
        float mirrorMatch = max(dot(V, reflectedGlint), 0.0);
        float bandWidth = clamp(uTerrainSunGlintBandWidth, 0.03, 0.90);
        float reflectionGate = smoothstep(1.0 - bandWidth, 1.0, mirrorMatch);
        float sunFacing = smoothstep(0.03, 0.36, ng);
        float horizonGate = smoothstep(0.015, 0.18, Lg.y);
        float viewGlance = smoothstep(0.08, 0.72, 1.0 - nv);
        float roughnessGate = mix(1.0, 0.25, smoothstep(0.35, 0.95, roughness));
        float band = pow(nh, max(uTerrainSunGlintSharpness, 1.0)) *
                     reflectionGate * sunFacing * horizonGate * viewGlance *
                     roughnessGate;
        terrainSunGlint = clamp(band * terrainSunGlintMask *
                                    max(uTerrainSunGlintIntensity, 0.0),
                                0.0, 3.0);
        specular *= terrainSpecularMul;
    }

    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;	

    float shadowFactor = 1.0 - shadow;
    if (uShadowBandEnabled) {
        float steps = max(1.0, float(uShadowBandSteps));
        float band = floor(shadowFactor * steps) / steps;
        shadowFactor = mix(band, shadowFactor, clamp(uShadowBandSoftness, 0.0, 1.0));
    }
    float lightTerm = NdotLs * shadowFactor;
    if (uToonEnabled) {
        float steps = max(1.0, float(uToonSteps));
        float t = floor(lightTerm * steps) / steps;
        lightTerm = max(uToonMin, t);
    }
    Lo += (kD * albedo / PI + specular) * sunRadiance * lightTerm;
    if (useTerrainMaterialProps && uTerrainSunGlintEnabled) {
        Lo += sunRadiance * terrainSunGlint * mix(0.65, 1.0, shadowFactor) * 0.11;
    }

    // ---------- FIRE (non-shadowed point light) ----------
    if (uHasFire)
    {
        vec3  toFire = uFirePos - FragPos;
        float dist   = length(toFire);
        vec3  Lf     = (dist > 0.0001) ? (toFire / dist) : vec3(0.0, 1.0, 0.0);
        vec3  Hf = normalize(Lf + V);
        vec3  fireDir = normalize(uFireDir);
        float forwardBias = clamp(dot(-Lf, fireDir), 0.0, 1.0);
        forwardBias = mix(0.35, 1.0, forwardBias * forwardBias);
        float groundBias = clamp(dot(Lf, vec3(0.0, 1.0, 0.0)), 0.0, 1.0);
        float fireField = fbm(FragPos.xz * 0.95 + vec2(uTime * 0.75, -uTime * 0.58));
        float fireRipple =
            0.5 + 0.5 * sin(dist * 5.2 - uTime * 9.4 + fireField * 6.28318);
        fireRipple = mix(0.78, 1.18, fireRipple);

        // Classic attenuation
        float attenuation = 1.0 / (uFireConstant + uFireLinear * dist + uFireQuadratic * (dist * dist));
        float flicker = 1.0 + uFireFlicker * sin(uTime * 17.0 + FragPos.x * 3.0 + FragPos.z * 2.0);
        float coreMask = clamp(1.0 - dist / (uFireAmbientRadius * 0.45), 0.0, 1.0);
        coreMask = coreMask * coreMask;
        float bounceMask = clamp(1.0 - dist / (uFireAmbientRadius * 1.35), 0.0, 1.0);
        bounceMask = bounceMask * bounceMask;
        vec3 coreColor = mix(uFireColor, vec3(1.0, 0.92, 0.72), 0.45);
        vec3 bounceColor = mix(uFireColor, vec3(0.40, 0.28, 0.18), 0.38);
        vec3 fireRadiance =
            coreColor * (uFireIntensity * attenuation * flicker * forwardBias);

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
        Lo += albedo * bounceColor *
              (uFireAmbient * 0.70 * bounceMask * groundBias * forwardBias *
               fireRipple);

        // Local ambient lift
        float amb = clamp(1.0 - dist / uFireAmbientRadius, 0.0, 1.0);
        amb = amb * amb;
        ambient += albedo *
                   (uFireAmbient * amb * 0.45 * forwardBias *
                    mix(0.85, 1.15, fireField)) * bounceColor;
        ambient += albedo * (uFireAmbient * 0.35 * coreMask) * coreColor;
    }

    vec3 emissive = toLinear(uEmissiveColor) * uEmissiveStrength;
    if (!useTerrainMaterialProps && uHasEmissiveMap) {
        emissive += texture(texEmissive, TexCoord).rgb * uEmissiveStrength;
    }
    emissive *= max(uEmissiveBoost, 0.0);
    if (uEmissiveFlicker > 0.0) {
        emissive *= (1.0 + uEmissiveFlicker *
                     sin(uTime * 6.0 + FragPos.x * 0.7 + FragPos.z * 0.7));
    }

    vec3 lit = ambient + Lo + emissive;

    if (uShowShadowCascades && uUseCascadedShadows) {
        float viewDepth = abs((uViewMatrix * vec4(FragPos, 1.0)).z);
        int cascadeIndex = chooseShadowCascade(viewDepth);
        vec3 cascadeTint = vec3(1.0);
        if (cascadeIndex == 0) cascadeTint = vec3(1.0, 0.84, 0.72);
        else if (cascadeIndex == 1) cascadeTint = vec3(0.74, 1.0, 0.78);
        else if (cascadeIndex == 2) cascadeTint = vec3(0.70, 0.82, 1.0);
        else cascadeTint = vec3(1.0, 0.72, 1.0);
        lit = mix(lit, lit * cascadeTint, 0.32);
    }

    if (uRimEnabled) {
        float rim = 1.0 - max(dot(N, V), 0.0);
        rim = pow(rim, max(uRimPower, 0.001));
        lit += uRimColor * (rim * uRimStrength);
    }

    lit = applyAerialPerspective(lit);
    lit *= max(uSceneExposure, 0.0);

    FragColor = vec4(lit, baseColor.a);
}
