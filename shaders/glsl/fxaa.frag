#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D uSceneTex;
uniform bool uEnableFXAA;
uniform float uInvResolutionX;
uniform float uInvResolutionY;
uniform float uSpanMax;
uniform float uReduceMin;
uniform float uReduceMul;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec3 rgbM = texture(uSceneTex, TexCoords).rgb;
    if (!uEnableFXAA) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }

    vec2 invRes = vec2(uInvResolutionX, uInvResolutionY);
    vec3 rgbNW = texture(uSceneTex, TexCoords + vec2(-1.0, -1.0) * invRes).rgb;
    vec3 rgbNE = texture(uSceneTex, TexCoords + vec2( 1.0, -1.0) * invRes).rgb;
    vec3 rgbSW = texture(uSceneTex, TexCoords + vec2(-1.0,  1.0) * invRes).rgb;
    vec3 rgbSE = texture(uSceneTex, TexCoords + vec2( 1.0,  1.0) * invRes).rgb;

    float lumaNW = luma(rgbNW);
    float lumaNE = luma(rgbNE);
    float lumaSW = luma(rgbSW);
    float lumaSE = luma(rgbSE);
    float lumaM = luma(rgbM);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    if (lumaMax - lumaMin < 0.01) {
        FragColor = vec4(rgbM, 1.0);
        return;
    }

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) *
                          (0.25 * uReduceMul), uReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-uSpanMax), vec2(uSpanMax)) * invRes;

    vec3 rgbA = 0.5 * (
        texture(uSceneTex, TexCoords + dir * (1.0 / 3.0 - 0.5)).rgb +
        texture(uSceneTex, TexCoords + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(uSceneTex, TexCoords + dir * -0.5).rgb +
        texture(uSceneTex, TexCoords + dir * 0.5).rgb);

    float lumaB = luma(rgbB);
    vec3 outColor = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
    FragColor = vec4(outColor, 1.0);
}
