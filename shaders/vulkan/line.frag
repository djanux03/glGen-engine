#version 450
// line.frag -- see line.vert for why the fade lives here and not there.

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 camPosFade; // xyz = camera world position, w = fade distance
} pc;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

void main() {
    const float fadeDistance = max(pc.camPosFade.w, 0.001);
    // Horizontal distance only: the grid is a ground plane, and including the
    // camera's height would shrink the visible patch every time you fly up,
    // which reads as the grid collapsing toward you.
    const vec2 delta = vWorldPos.xz - pc.camPosFade.xz;
    const float d = length(delta);

    // Squared falloff: linear leaves a visible ring where the grid ends,
    // squared reaches zero gently enough that the edge is invisible.
    float fade = clamp(1.0 - d / fadeDistance, 0.0, 1.0);
    fade *= fade;

    const float alpha = vColor.a * fade;
    if (alpha < 0.002)
        discard;
    outColor = vec4(vColor.rgb, alpha);
}
