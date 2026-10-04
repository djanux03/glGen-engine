#version 460
#extension GL_GOOGLE_include_directive : require
#include "frameData.glsl"
#include "atmosphereData.glsl"
#include "skyModel.glsl"
#include "skyLutSample.glsl"
#define FOG_AERIAL_ONLY
#include "fog.glsl"
layout(set=3,binding=0) uniform samplerCube uEnvironment;
#include "fogSample.glsl"
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
void main() {
    vec4 scene=texture(uUnfoggedScene,vUV);
    if(uAtmosphere.dustAlbedo.w>=0) scene.rgb=vec3(uAtmosphere.dustAlbedo.w);
    float depth=texture(uFogDepth,vUV).r;
    vec4 p=uAtmosphere.invSurfaceViewProj*vec4(vUV*2.0-1.0,depth,1);
    vec3 position=p.xyz/p.w;
    bool sky=depth>=.999999;
    if(sky) position=uAtmosphere.camera.xyz+fogRay(vUV)*4000.0;
    outColor=vec4(applyCameraAtmosphere(scene.rgb,position,sky),scene.a);
}
