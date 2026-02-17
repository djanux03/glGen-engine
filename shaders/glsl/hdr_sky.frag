#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uHDR;
uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform float uExposure;
uniform float uGamma;

// solid sky
uniform bool uUseSolidSky;
uniform vec3 uSkyTop;
uniform vec3 uSkyHorizon;


uniform mat3 uSkyRot; // rotates worldDir before sampling

const float PI = 3.14159265359;



vec2 dirToEquirectUV(vec3 d)
{
    d = normalize(d);
    float u = atan(d.z, d.x) / (2.0 * PI) + 0.5;
    float v = asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5;
    return vec2(u, v);
}

void main()
{
    // Reconstruct view ray in view space from screen UV
    vec2 ndc = vUV * 2.0 - 1.0;
    vec4 clip = vec4(ndc, 1.0, 1.0);

    vec4 viewPos = uInvProj * clip;
    viewPos /= viewPos.w;

    vec3 viewDir = normalize(viewPos.xyz);

    // Rotate into world (w=0 so translation is ignored)
    vec3 worldDir = normalize((uInvView * vec4(viewDir, 0.0)).xyz);

    // Apply user sky rotation (XYZ) then sample equirect HDR
    worldDir = normalize(uSkyRot * worldDir);
vec3 mapped;

if (uUseSolidSky)
{
    // Simple sky gradient based on direction Y
    float t = clamp(worldDir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 col = mix(uSkyHorizon, uSkyTop, t);

    // Keep the same “output gamma” behavior as your HDR path
    mapped = pow(col, vec3(1.0 / uGamma));
}
else
{
    vec2 uv = dirToEquirectUV(worldDir);
    vec3 hdr = texture(uHDR, uv).rgb;

    // Tone mapping (exposure) + gamma
    mapped = vec3(1.0) - exp(-hdr * uExposure);
    mapped = pow(mapped, vec3(1.0 / uGamma));
}

FragColor = vec4(mapped, 1.0);

}
