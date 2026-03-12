#version 330 core
out vec4 FragColor;
in vec2 TexCoords;

uniform sampler2D uCurrentColor;
uniform sampler2D uHistoryColor;
uniform sampler2D uDepthTex;
uniform float uHistoryBlend;
uniform float uInvResolutionX;
uniform float uInvResolutionY;
uniform float uHistoryUVOffsetX;
uniform float uHistoryUVOffsetY;

void main() {
    vec2 invRes = vec2(uInvResolutionX, uInvResolutionY);
    vec3 current = texture(uCurrentColor, TexCoords).rgb;

    vec3 cN = texture(uCurrentColor, TexCoords + vec2(0.0, invRes.y)).rgb;
    vec3 cS = texture(uCurrentColor, TexCoords - vec2(0.0, invRes.y)).rgb;
    vec3 cE = texture(uCurrentColor, TexCoords + vec2(invRes.x, 0.0)).rgb;
    vec3 cW = texture(uCurrentColor, TexCoords - vec2(invRes.x, 0.0)).rgb;

    vec3 nMin = min(current, min(min(cN, cS), min(cE, cW)));
    vec3 nMax = max(current, max(max(cN, cS), max(cE, cW)));

    vec2 historyUV = clamp(TexCoords + vec2(uHistoryUVOffsetX, uHistoryUVOffsetY),
                           vec2(0.0), vec2(1.0));
    vec3 history = texture(uHistoryColor, historyUV).rgb;
    vec3 clampedHistory = clamp(history, nMin, nMax);

    float depthC = texture(uDepthTex, TexCoords).r;
    float depthN = texture(uDepthTex, TexCoords + vec2(0.0, invRes.y)).r;
    float depthS = texture(uDepthTex, TexCoords - vec2(0.0, invRes.y)).r;
    float depthE = texture(uDepthTex, TexCoords + vec2(invRes.x, 0.0)).r;
    float depthW = texture(uDepthTex, TexCoords - vec2(invRes.x, 0.0)).r;
    float depthEdge = abs(depthC - 0.25 * (depthN + depthS + depthE + depthW));
    depthEdge = clamp(depthEdge * 80.0, 0.0, 1.0);

    float colorDelta = length(current - clampedHistory);
    float reject = clamp(colorDelta * 1.8 + depthEdge, 0.0, 1.0);

    float historyWeight = clamp(uHistoryBlend * (1.0 - reject), 0.0, 0.98);
    vec3 resolved = mix(current, clampedHistory, historyWeight);
    FragColor = vec4(resolved, 1.0);
}
