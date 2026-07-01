#version 330 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;

uniform sampler2D shadowMap;
uniform sampler2DArray uShadowMapArray;
uniform mat4 uLightSpaceMatrix;
uniform mat4 uLightSpaceMatrices[4];
uniform mat4 uViewMatrix;
uniform vec3 uLightDir;
uniform float uShadowStrength;
uniform bool uShadowsEnabled;
uniform bool uUseCascadedShadows;
uniform int uCascadeCount;
uniform float uCascadeSplits[4];
uniform float uShadowNormalBias;
uniform float uShadowDepthBias;
uniform float uShadowSoftness;
uniform bool uShowShadowCascades;

uniform vec3 uSunColor;
uniform float uSunIntensity;
uniform float uAmbient;
uniform vec3 uCameraPos;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform bool uAerialPerspectiveEnabled;
uniform float uAerialPerspectiveDensity;
uniform float uAerialPerspectiveStart;
uniform float uAerialPerspectiveHeightFalloff;
uniform float uAerialPerspectiveSkyBlend;
uniform float uAerialPerspectiveSunGlow;
uniform float uAerialPerspectiveDesaturation;
uniform vec3 uAerialHorizonColor;
uniform vec3 uAerialZenithColor;
uniform bool uAmbientHemiEnabled;
uniform float uAmbientHemiIntensity;
uniform float uAmbientHorizonStrength;
uniform float uAmbientTerrainBoost;
uniform vec3 uAmbientSkyColor;
uniform vec3 uAmbientHorizonColor;
uniform vec3 uAmbientGroundColor;
uniform vec3 uTerrainFlatGreenColor;
uniform bool uHasFire;
uniform vec3 uFirePos;
uniform vec3 uFireDir;
uniform vec3 uFireColor;
uniform float uFireIntensity;
uniform float uFireConstant;
uniform float uFireLinear;
uniform float uFireQuadratic;
uniform float uFireFlicker;
uniform float uFireAmbient;
uniform float uFireAmbientRadius;
uniform float uTime;

float rand(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = rand(i);
    float b = rand(i + vec2(1.0, 0.0));
    float c = rand(i + vec2(0.0, 1.0));
    float d = rand(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) +
           (d - b) * u.x * u.y;
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; ++i) {
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

float sampleShadow2D(vec4 fragPosLightSpace, vec3 normal) {
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;
    float currentDepth = projCoords.z;
    vec3 lightDir = normalize(-uLightDir);
    float slopeBias = max(0.0, 1.0 - dot(normal, lightDir));
    float bias = max(uShadowDepthBias + uShadowNormalBias * slopeBias,
                     uShadowDepthBias);
    float shadow = 0.0;
    vec2 texelSize = (1.0 / vec2(textureSize(shadowMap, 0))) *
                     max(0.25, uShadowSoftness);
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap,
                                     projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    shadow /= 9.0;
    return shadow;
}

float sampleShadowCascade(int cascadeIndex, vec3 normal) {
    vec4 fragPosLightSpace =
        uLightSpaceMatrices[cascadeIndex] * vec4(FragPos, 1.0);
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;

    float currentDepth = projCoords.z;
    vec3 lightDir = normalize(-uLightDir);
    float slopeBias = max(0.0, 1.0 - dot(normal, lightDir));
    float bias = max(uShadowDepthBias + uShadowNormalBias * slopeBias,
                     uShadowDepthBias);
    bias *= 1.0 + float(cascadeIndex) * 0.18;

    ivec3 mapSize = textureSize(uShadowMapArray, 0);
    vec2 texelSize = 1.0 / vec2(max(mapSize.x, 1), max(mapSize.y, 1));
    texelSize *= max(0.25, uShadowSoftness *
                           (1.0 + float(cascadeIndex) * 0.35));

    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
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
    if (!uShadowsEnabled || uShadowStrength <= 0.0)
        return 0.0;
    vec3 normal = normalize(Normal);
    if (!uUseCascadedShadows)
        return sampleShadow2D(fragPosLightSpace, normal) * uShadowStrength;

    float viewDepth = abs((uViewMatrix * vec4(FragPos, 1.0)).z);
    int cascadeIndex = chooseShadowCascade(viewDepth);
    float shadow = sampleShadowCascade(cascadeIndex, normal);
    if (cascadeIndex + 1 < uCascadeCount) {
        float prevSplit = cascadeIndex == 0 ? 0.0 : uCascadeSplits[cascadeIndex - 1];
        float split = uCascadeSplits[cascadeIndex];
        float blendRange = max((split - prevSplit) * 0.12, 2.0);
        float blend = smoothstep(split - blendRange, split, viewDepth);
        shadow = mix(shadow, sampleShadowCascade(cascadeIndex + 1, normal),
                     blend);
    }
    float lastSplit = uCascadeSplits[max(uCascadeCount - 1, 0)];
    float fade = 1.0 - smoothstep(lastSplit * 0.92, lastSplit, viewDepth);
    return shadow * fade * uShadowStrength;
}

vec3 toneMapReinhard(vec3 c) { return c / (c + vec3(1.0)); }
vec3 toSRGB(vec3 lin) { return pow(max(lin, vec3(0.0)), vec3(1.0 / 2.2)); }

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

vec3 ambientHemisphere(vec3 normal) {
    if (!uAmbientHemiEnabled) {
        return vec3(uAmbient);
    }

    float upT = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 hemi = mix(uAmbientGroundColor, uAmbientSkyColor, upT);
    float horizon = pow(1.0 - abs(clamp(normal.y, -1.0, 1.0)), 1.35);
    hemi = mix(hemi, uAmbientHorizonColor,
               clamp(horizon * uAmbientHorizonStrength, 0.0, 1.0));
    return hemi * uAmbient * max(uAmbientHemiIntensity, 0.0) *
           max(uAmbientTerrainBoost, 0.0);
}

void main()
{
    vec3 N = normalize(Normal);
    vec3 L = normalize(-uLightDir);
    float NdotL = max(dot(N, L), 0.0);

    float shadow = 0.0;
    if (NdotL > 0.0) {
        shadow = ShadowDirectional(uLightSpaceMatrix * vec4(FragPos, 1.0));
        shadow = clamp(shadow, 0.0, 1.0);
    }

    vec3 albedo = uTerrainFlatGreenColor;
    vec3 sunRadiance = uSunColor * uSunIntensity;
    vec3 ambient = albedo * ambientHemisphere(N);

    float lightTerm = NdotL * (1.0 - shadow);
    vec3 lit = ambient + albedo * sunRadiance * lightTerm;

    if (uShowShadowCascades && uUseCascadedShadows) {
        float viewDepth = abs((uViewMatrix * vec4(FragPos, 1.0)).z);
        int cascadeIndex = chooseShadowCascade(viewDepth);
        vec3 cascadeTint = cascadeIndex == 0 ? vec3(1.0, 0.84, 0.72) :
                           cascadeIndex == 1 ? vec3(0.74, 1.0, 0.78) :
                           cascadeIndex == 2 ? vec3(0.70, 0.82, 1.0) :
                                               vec3(1.0, 0.72, 1.0);
        lit = mix(lit, lit * cascadeTint, 0.32);
    }

    if (uHasFire) {
        vec3 toFire = uFirePos - FragPos;
        float dist = length(toFire);
        vec3 Lf = (dist > 0.0001) ? (toFire / dist) : vec3(0.0, 1.0, 0.0);
        vec3 fireDir = normalize(uFireDir);
        float forwardBias = clamp(dot(-Lf, fireDir), 0.0, 1.0);
        forwardBias = mix(0.35, 1.0, forwardBias * forwardBias);
        float groundBias = clamp(dot(Lf, vec3(0.0, 1.0, 0.0)), 0.0, 1.0);
        float NdotLf = max(dot(N, Lf), 0.0);
        float attenuation = 1.0 / (uFireConstant + uFireLinear * dist +
                                   uFireQuadratic * (dist * dist));
        float flicker = 1.0 + uFireFlicker *
                        sin(uTime * 17.0 + FragPos.x * 3.0 + FragPos.z * 2.0);
        float coreMask = clamp(1.0 - dist / (uFireAmbientRadius * 0.45), 0.0, 1.0);
        coreMask = coreMask * coreMask;
        float bounceMask = clamp(1.0 - dist / (uFireAmbientRadius * 1.45), 0.0, 1.0);
        bounceMask = bounceMask * bounceMask;
        vec3 coreColor = mix(uFireColor, vec3(1.0, 0.92, 0.72), 0.45);
        vec3 bounceColor = mix(uFireColor, vec3(0.40, 0.28, 0.18), 0.38);
        vec3 fireRadiance =
            coreColor * (uFireIntensity * attenuation * flicker * forwardBias);
        lit += albedo * fireRadiance * NdotLf;
        lit += albedo * bounceColor *
               (uFireAmbient * 0.95 * bounceMask * groundBias * forwardBias);

        float amb = clamp(1.0 - dist / uFireAmbientRadius, 0.0, 1.0);
        amb = amb * amb;
        lit += albedo * (uFireAmbient * amb * 0.45 * forwardBias) * bounceColor;
        lit += albedo * (uFireAmbient * 0.40 * coreMask) * coreColor;
    }



    lit = applyAerialPerspective(lit);

    FragColor = vec4(lit, 1.0);
}
