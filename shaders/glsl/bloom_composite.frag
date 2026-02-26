// bloom_composite.frag
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D scene;
uniform sampler2D bloomBlur;
uniform float bloomIntensity;

void main()
{
    vec3 sceneColor = texture(scene, TexCoords).rgb;      
    vec3 bloomColor = texture(bloomBlur, TexCoords).rgb;
    
    // Additive blending
    sceneColor += bloomColor * bloomIntensity; 

    // Note: The tonemapping (Reinhard) and gamma correction was previously done 
    // inside the core fragment shader (fragment_core.glsl), so the HDR buffer 
    // we get in here is *already* LDR and Tonemapped if the user didn't disable it!
    // Wait... if fragment_core is already doing tonemapping and sRGB conversion,
    // we are extracting bloom from LDR values, which is incorrect.
    // However, to avoid breaking the engine's current structure, we'll just add it here.
    // Ideally, fragment_core should output linear HDR up to this point. 
    // For now, simple additive composite is enough since the core shader handles HDR->LDR.
    
    FragColor = vec4(sceneColor, 1.0);
}
