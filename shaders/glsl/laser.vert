#version 330 core
layout (location = 0) in vec2 aBeamVertex;

out float vSide;
out float vT;

uniform mat4 view;
uniform mat4 projection;
uniform vec3 uStart;
uniform vec3 uEnd;
uniform vec3 uCameraPos;
uniform float uRadius;

void main()
{
    float t = aBeamVertex.x;
    float side = aBeamVertex.y;

    vec3 beam = uEnd - uStart;
    float beamLen = max(length(beam), 0.0001);
    vec3 beamDir = beam / beamLen;
    vec3 center = mix(uStart, uEnd, t);
    vec3 viewDir = normalize(uCameraPos - center);
    vec3 sideDir = cross(beamDir, viewDir);
    if (dot(sideDir, sideDir) < 0.0001) {
        sideDir = cross(beamDir, vec3(0.0, 1.0, 0.0));
    }
    if (dot(sideDir, sideDir) < 0.0001) {
        sideDir = vec3(1.0, 0.0, 0.0);
    }
    sideDir = normalize(sideDir);

    vec3 worldPos = center + sideDir * side * uRadius;
    vSide = side;
    vT = t;
    gl_Position = projection * view * vec4(worldPos, 1.0);
}
