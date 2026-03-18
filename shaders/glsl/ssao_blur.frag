// ssao_blur.frag
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D ssaoInput;
uniform sampler2D depthTex;
uniform float uUpscale;
uniform float uNearPlane;
uniform float uFarPlane;

float linearizeDepth(float d) {
    float z = d * 2.0 - 1.0;
    return (2.0 * uNearPlane * uFarPlane) /
           max(uFarPlane + uNearPlane - z * (uFarPlane - uNearPlane), 0.0001);
}

void main() {
    vec2 aoTexel = 1.0 / vec2(textureSize(ssaoInput, 0));
    vec2 depthTexel = 1.0 / vec2(textureSize(depthTex, 0));
    float centerDepth = texture(depthTex, TexCoords).r;
    float centerLin = linearizeDepth(centerDepth);
    float result = 0.0;
    float weightSum = 0.0;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 sampleOffset = vec2(float(x), float(y));
            vec2 aoUV = clamp(TexCoords + sampleOffset * aoTexel, vec2(0.0), vec2(1.0));
            vec2 depthUV = clamp(TexCoords + sampleOffset * depthTexel, vec2(0.0), vec2(1.0));
            float ao = texture(ssaoInput, aoUV).r;
            float d = texture(depthTex, depthUV).r;
            float dl = linearizeDepth(d);
            float w = 1.0 / (1.0 + float(x * x + y * y));
            float depthWeight = exp(-abs(dl - centerLin) * 0.05);
            float weight = w * depthWeight;
            result += ao * weight;
            weightSum += weight;
        }
    }

    result /= max(weightSum, 0.0001);
    FragColor = vec4(vec3(result), 1.0);
}
