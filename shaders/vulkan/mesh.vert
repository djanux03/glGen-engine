#version 450
#extension GL_GOOGLE_include_directive : require

// Scene vertex stage. Static scene (model = identity), so world position is the
// input position. Outputs the view-space depth used to pick a shadow cascade.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 vNormalWS;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out float vViewZ;

#include "frameData.glsl"

layout(push_constant) uniform Push {
    mat4 model;        // per-instance transform
    uint textureIndex, roughnessIndex, metallicIndex, aoIndex;
    uint normalIndex, opacityIndex;
    float roughnessScalar, metallicScalar, aoScalar, alphaCutoff;
    uint materialFlags;
    float unused0, unused1, unused2, unused3, viewmodelFov;
} pc;

void main() {
    vec4 world = pc.model * vec4(inPos, 1.0);
    mat3 deformation = mat3(pc.model);
    // Keep forearm bases attached below/behind the camera while a hand follows
    // the magazine or bolt. Rigidly moving the entire arm exposed its cut end
    // during reload. The wrist and fingers stay rigid; only the forearm blends.
    if ((pc.materialFlags & (1u<<15)) != 0u) {
        bool rightArm = (pc.materialFlags & (1u<<16)) != 0u;
        vec3 base = rightArm ? vec3(.203,-.158,.218) : vec3(-.127,-.161,.370);
        vec3 shoulder = rightArm ? vec3(.32,-.48,.18) : vec3(-.32,-.48,.18);
        vec3 delta = (inverse(uFrame.view)*vec4(shoulder,1)).xyz
                   - (pc.model*vec4(base,1)).xyz;
        float start=.075, end=rightArm ? .21 : .36;
        float t=clamp((inPos.z-start)/(end-start),0.0,1.0);
        world.xyz += delta*t*t*(3.0-2.0*t);
        deformation[2] += delta*(6.0*t*(1.0-t)/(end-start));
    }
    gl_Position = uFrame.viewProj * world;
    // Foreground uses an unjittered projection, avoiding a vibrating sight.
    if ((pc.materialFlags & (1u<<14)) != 0u) {
        mat4 projection = uFrame.viewProj * inverse(uFrame.view);
        projection[2][0]=0.0; projection[2][1]=0.0;
        float aspect=abs(projection[1][1]/projection[0][0]);
        float focal=1.0/tan(radians(pc.viewmodelFov)*0.5);
        projection[0][0]=focal/aspect; projection[1][1]=-focal;
        gl_Position = projection * uFrame.view * world;
    }
    vNormalWS = transpose(inverse(deformation)) * inNormal;
    vUV = inUV;
    vWorldPos = world.xyz;
    vViewZ = -(uFrame.view * world).z; // positive distance in front of camera
}
