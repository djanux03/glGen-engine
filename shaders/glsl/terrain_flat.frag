#version 330 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;

uniform sampler2D shadowMap;
uniform mat4 uLightSpaceMatrix;
uniform vec3 uLightDir;
uniform float uShadowStrength;

uniform vec3 uSunColor;
uniform float uSunIntensity;
uniform float uAmbient;
uniform vec3 uCameraPos;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
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

float ShadowDirectional(vec4 fragPosLightSpace) {
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;
    float currentDepth = projCoords.z;
    vec3 normal = normalize(Normal);
    vec3 lightDir = normalize(-uLightDir);
    float bias = max(0.002 * (1.0 - dot(normal, lightDir)), 0.0005);
    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap,
                                     projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    shadow /= 9.0;
    return shadow * uShadowStrength;
}

vec3 toneMapReinhard(vec3 c) { return c / (c + vec3(1.0)); }
vec3 toSRGB(vec3 lin) { return pow(max(lin, vec3(0.0)), vec3(1.0 / 2.2)); }

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
    vec3 ambient = albedo * uAmbient;

    float lightTerm = NdotL * (1.0 - shadow);
    vec3 lit = ambient + albedo * sunRadiance * lightTerm;

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



    float dist = length(uCameraPos - FragPos);
    float heightDensity = uFogDensity * exp(-max(FragPos.y, 0.0) * uFogHeightFalloff);
    float fogFactor = exp(-pow(dist * heightDensity, 2.0));
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    lit = mix(uFogColor, lit, fogFactor);

    FragColor = vec4(lit, 1.0);
}
