#version 450

// Procedural atmospheric sky. Reconstructs the per-pixel world view ray from the
// inverse view-projection, then shades an analytic sky that responds to the sun
// elevation: blue day gradient, dusk reddening near the horizon, and a darkened
// night sky as the sun drops. Includes a sun disc + glow. Writes linear HDR.
layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 invViewProj;
    vec4 sunDir; // xyz = direction the light travels (downward); toward-sun = -sunDir
    vec4 camPos;
} pc;

vec3 skyColor(vec3 dir, vec3 sun) {
    float up = clamp(dir.y, 0.0, 1.0);

    // Daytime gradient: horizon -> zenith.
    vec3 zenith = vec3(0.10, 0.26, 0.58);
    vec3 horizon = vec3(0.62, 0.74, 0.86);
    vec3 sky = mix(horizon, zenith, pow(up, 0.5));

    // Sun elevation drives day / dusk / night.
    float sunEl = sun.y;
    float day = smoothstep(-0.10, 0.18, sunEl);
    vec3 night = vec3(0.02, 0.03, 0.06);
    vec3 duskTint = vec3(0.95, 0.45, 0.22);
    float dusk = (1.0 - day) * smoothstep(0.0, 0.4, up) *
                 smoothstep(-0.25, 0.12, sunEl);
    sky = mix(night, sky, day);
    sky = mix(sky, duskTint, dusk * 0.6);

    // Sun disc + glow.
    float cosA = max(dot(dir, sun), 0.0);
    float disc = smoothstep(0.9990, 0.9996, cosA);
    float glow = pow(cosA, 350.0) * 0.6 + pow(cosA, 12.0) * 0.15;
    vec3 sunCol = mix(vec3(1.0, 0.55, 0.25), vec3(1.0, 0.96, 0.88), day);
    sky += sunCol * (disc * 12.0 + glow) * max(day, 0.15);

    // Below the horizon: ground haze.
    sky = mix(sky, mix(vec3(0.06, 0.07, 0.08), sky * 0.5, day),
              smoothstep(0.0, -0.15, dir.y));
    return max(sky, vec3(0.0));
}

void main() {
    vec4 farW = pc.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 worldFar = farW.xyz / farW.w;
    vec3 dir = normalize(worldFar - pc.camPos.xyz);
    vec3 sun = normalize(-pc.sunDir.xyz);
    outColor = vec4(skyColor(dir, sun), 1.0);
}
