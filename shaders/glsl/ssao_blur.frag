// ssao_blur.frag
#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D ssaoInput;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(ssaoInput, 0));
    float result = 0.0;
    float weightSum = 0.0;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 off = vec2(float(x), float(y)) * texel;
            float w = 1.0 / (1.0 + float(x * x + y * y));
            result += texture(ssaoInput, TexCoords + off).r * w;
            weightSum += w;
        }
    }

    result /= max(weightSum, 0.0001);
    FragColor = vec4(vec3(result), 1.0);
}
