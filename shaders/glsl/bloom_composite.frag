// bloom_composite.frag
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D scene;
uniform sampler2D bloomBlur;
uniform sampler2D ssaoTex;
uniform sampler2D volumetricTex;
uniform sampler2D depthTex;
uniform float bloomIntensity;
uniform float uBrightness;
uniform bool uEnableSSAO;
uniform bool uEnableVolumetric;
uniform bool uEnableOutline;
uniform float uOutlineStrength;
uniform float uOutlineThreshold;
uniform float uOutlineThickness;
uniform vec3 uOutlineColor;
uniform float uInvResolutionX;
uniform float uInvResolutionY;
uniform bool uEnableDistanceTint;
uniform float uDistanceTintStart;
uniform float uDistanceTintEnd;
uniform vec3 uDistanceTintColor;
uniform float uNearPlane;
uniform float uFarPlane;
uniform bool uEnableColorGrade;
uniform float uGradeSaturation;
uniform float uGradeContrast;
uniform float uGradeLift;
uniform float uGradeGamma;
uniform float uGradeGain;
uniform vec3 uGradeTint;
uniform bool uEnablePalette;
uniform int uPaletteSteps;

vec3 applyColorGrade(vec3 c)
{
    // Saturation
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(luma), c, uGradeSaturation);

    // Contrast (pivot around 0.5)
    c = (c - 0.5) * uGradeContrast + 0.5;

    // Lift/Gamma/Gain
    c = c + vec3(uGradeLift);
    c = pow(max(c, vec3(0.0)), vec3(1.0 / max(uGradeGamma, 0.001)));
    c = c * vec3(uGradeGain);

    // Tint
    c *= uGradeTint;
    return clamp(c, 0.0, 1.0);
}

vec3 applyPaletteQuantize(vec3 c)
{
    float steps = max(2.0, float(uPaletteSteps));
    return floor(c * steps + 0.5) / steps;
}

void main()
{
    vec3 sceneColor = texture(scene, TexCoords).rgb;      
    vec3 bloomColor = texture(bloomBlur, TexCoords).rgb;
    float ao = texture(ssaoTex, TexCoords).r;
    vec3 volumetric = texture(volumetricTex, TexCoords).rgb;
    
    if (uEnableSSAO) {
        // Clamp AO to avoid over-darkening stylized assets.
        sceneColor *= clamp(ao, 0.35, 1.0);
    }

    sceneColor += bloomColor * bloomIntensity;
    if (uEnableVolumetric) {
        sceneColor += volumetric;
    }

    if (uEnableOutline) {
        float depthC = texture(depthTex, TexCoords).r;
        vec2 texel = vec2(uInvResolutionX, uInvResolutionY) * uOutlineThickness;
        float depthL = texture(depthTex, TexCoords + vec2(-texel.x, 0.0)).r;
        float depthR = texture(depthTex, TexCoords + vec2( texel.x, 0.0)).r;
        float depthU = texture(depthTex, TexCoords + vec2(0.0, -texel.y)).r;
        float depthD = texture(depthTex, TexCoords + vec2(0.0,  texel.y)).r;
        float edge = max(max(abs(depthC - depthL), abs(depthC - depthR)),
                         max(abs(depthC - depthU), abs(depthC - depthD)));
        float outline = smoothstep(uOutlineThreshold, uOutlineThreshold * 2.0, edge);
        sceneColor = mix(sceneColor, uOutlineColor, outline * uOutlineStrength);
    }

    if (uEnableDistanceTint) {
        float depth = texture(depthTex, TexCoords).r;
        // Reconstruct view-space z from depth buffer
        float z = depth * 2.0 - 1.0;
        float viewZ = (2.0 * uNearPlane * uFarPlane) /
                      (uFarPlane + uNearPlane - z * (uFarPlane - uNearPlane));
        float t = smoothstep(uDistanceTintStart, uDistanceTintEnd, viewZ);
        sceneColor = mix(sceneColor, uDistanceTintColor, t);
    }

    if (uEnableColorGrade) {
        sceneColor = applyColorGrade(sceneColor);
    }

    if (uEnablePalette) {
        sceneColor = applyPaletteQuantize(sceneColor);
    }

    sceneColor *= uBrightness;

    // ACES Tonemapping
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    sceneColor = clamp((sceneColor * (a * sceneColor + b)) / (sceneColor * (c * sceneColor + d) + e), 0.0, 1.0);

    // Gamma Correction
    sceneColor = pow(sceneColor, vec3(1.0 / 2.2));

    FragColor = vec4(sceneColor, 1.0);
}
