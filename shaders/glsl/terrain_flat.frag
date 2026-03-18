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
uniform vec3 uTerrainFlatGreenColor;

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

    lit = toneMapReinhard(lit);
    lit = toSRGB(lit);

    float dist = length(uCameraPos - FragPos);
    float fogFactor = exp(-pow(dist * uFogDensity, 2.0));
    fogFactor = clamp(fogFactor, 0.0, 1.0);
    lit = mix(uFogColor, lit, fogFactor);

    FragColor = vec4(lit, 1.0);
}
