#version 450

// Fullscreen triangle for the sky background. Emits clip-space NDC so the
// fragment shader can reconstruct a world-space view ray via the inverse
// view-projection. Positioned at the far plane (z = 1, w = 1).
layout(location = 0) out vec2 vNdc;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vec2 ndc = p * 2.0 - 1.0;
    vNdc = ndc;
    gl_Position = vec4(ndc, 1.0, 1.0);
}
