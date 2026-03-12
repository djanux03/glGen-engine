// bloom_composite.frag
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D scene;
uniform sampler2D bloomBlur;
uniform sampler2D ssaoTex;
uniform sampler2D volumetricTex;
uniform float bloomIntensity;
uniform float uBrightness;
uniform bool uEnableSSAO;
uniform bool uEnableVolumetric;

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

    sceneColor *= uBrightness;

    FragColor = vec4(sceneColor, 1.0);
}
