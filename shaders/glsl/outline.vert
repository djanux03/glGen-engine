#version 410 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

// Uniforms
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

uniform float outlineScale = 0.015; // Represents screen width percentage

void main()
{
    vec4 clipPos = projection * view * model * vec4(aPos, 1.0);
    
    // Transform normal to clip space
    mat3 normalMat = mat3(transpose(inverse(view * model)));
    vec3 viewNormal = normalize(normalMat * aNormal);
    vec4 clipNormal = projection * vec4(viewNormal, 0.0);
    
    // Expand geometry in normalized device coordinates (scaled by w for perspective)
    if (length(clipNormal.xy) > 0.0001) {
        clipPos.xy += normalize(clipNormal.xy) * outlineScale * clipPos.w;
    }
    
    gl_Position = clipPos;
}
