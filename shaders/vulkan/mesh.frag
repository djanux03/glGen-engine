#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require
#extension GL_GOOGLE_include_directive : require

layout(location=0) in vec3 vNormalWS;
layout(location=1) in vec2 vUV;
layout(location=2) in vec3 vWorldPos;
layout(location=3) in float vViewZ;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D uTextures[];
#include "frameData.glsl"
#include "paintMaterial.glsl"
layout(set=2,binding=0) uniform accelerationStructureEXT uTLAS;
layout(set=3,binding=0) uniform sampler2D uSSAO;
layout(set=4,binding=0) uniform samplerCube uEnvMap;

layout(push_constant) uniform Push {
    mat4 model;
    uint textureIndex, roughnessIndex, metallicIndex, aoIndex;
    float roughnessScalar, metallicScalar, aoScalar;
    uint materialFlags;
} pc;

float traceShadow(vec3 o, vec3 d) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q,uTLAS,gl_RayFlagsTerminateOnFirstHitEXT|
        gl_RayFlagsOpaqueEXT|gl_RayFlagsSkipClosestHitShaderEXT,
        0xffu,o,0.002,d,420.0);
    rayQueryProceedEXT(q);
    return rayQueryGetIntersectionTypeEXT(q,true)==
        gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
float shadowVis(vec3 o, vec3 L) {
    float r=uFrame.lightParams.z;
    int n=clamp(int(uFrame.lightParams.w),1,4);
    if(r<=0.0005||n==1) return traceShadow(o,L);
    vec3 t=normalize(cross(L,abs(L.y)<0.9?vec3(0,1,0):vec3(1,0,0)));
    vec3 b=cross(L,t); float s=0.0;
    for(int i=0;i<n;i++){float a=paintHash13(vWorldPos+float(i)*17.17)*6.2831853;
      float rr=sqrt(paintHash13(vWorldPos.zyx+float(i)*31.31))*r;
      s+=traceShadow(o,normalize(L+(cos(a)*t+sin(a)*b)*rr));}
    return s/float(n);
}

void main() {
    vec3 N=normalize(vNormalWS), L=normalize(-uFrame.lightDir.xyz);
    vec3 V=normalize(uFrame.camPosWS.xyz-vWorldPos);
    float fog=(1.0-exp(-max(vViewZ-uFrame.fogParams.y,0.0)*uFrame.fogParams.x));
    fog*=exp(-max(vWorldPos.y-uFrame.miscParams.y,0.0)*uFrame.fogParams.w);
    float fogAmt=clamp(fog,0.0,uFrame.fogParams.z);
    float vis=1.0;
    if(dot(N,L)>0.0&&fogAmt<0.85*uFrame.fogParams.z)
        vis=shadowVis(vWorldPos+N*0.02,L);
    vis=mix(1.0,vis,uFrame.lightParams.y);
    vec3 albedo=texture(uTextures[nonuniformEXT(pc.textureIndex)],vUV).rgb;
    float ssao=texture(uSSAO,gl_FragCoord.xy/vec2(textureSize(uSSAO,0))).r;
    vec3 irr=textureLod(uEnvMap,N,uFrame.iblParams.x).rgb;
    vec3 color=physicalPaintSurface(albedo,N,V,L,pc.roughnessScalar,
                                    pc.metallicScalar,vis,ssao,irr,1.0,1.0);
    vec3 viewDir=normalize(vWorldPos-uFrame.camPosWS.xyz);
    vec3 fogColor=textureLod(uEnvMap,vec3(viewDir.x,max(viewDir.y,0.04),viewDir.z),1.0).rgb;
    color=mix(color,fogColor,fogAmt);
    int dbg=int(uFrame.miscParams.x+0.5);
    if(dbg==1) color=albedo; else if(dbg==2) color=N*0.5+0.5;
    else if(dbg==3) color=vec3(fogAmt); else if(dbg==4) color=vec3(ssao);
    else if(dbg==5) color=vec3(vis); else if(dbg==6)
      color=(any(isnan(color))||any(isinf(color)))?vec3(1,0,1):vec3(0);
    outColor=vec4(max(color,vec3(0)),1);
}
