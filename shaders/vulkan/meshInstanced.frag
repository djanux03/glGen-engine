#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec3 vNormalWS;
layout(location=1) in vec2 vUV;
layout(location=2) in vec3 vWorldPos;
layout(location=3) in float vViewZ;
layout(location=4) in vec3 vColorJitter;
layout(location=5) in vec3 vBiomeWeights;
layout(location=6) in float vHeightFrac;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D uTextures[];
#include "frameData.glsl"
#include "biomeLighting.glsl"
#include "paintMaterial.glsl"
layout(set=2,binding=0) uniform accelerationStructureEXT uTLAS;
layout(set=3,binding=0) uniform sampler2D uSSAO;
layout(set=4,binding=0) uniform samplerCube uEnvMap;
layout(push_constant) uniform Push {
 mat4 model; uint textureIndex,roughnessIndex,metallicIndex,aoIndex;
 float roughnessScalar,metallicScalar,aoScalar; uint materialFlags;
 float windStrength,windSpeed,windMeshHeight,groundOcclusion,foliageSssStrength;
} pc;
float traceShadow(vec3 o,vec3 d){rayQueryEXT q;rayQueryInitializeEXT(q,uTLAS,
 gl_RayFlagsTerminateOnFirstHitEXT|gl_RayFlagsOpaqueEXT|
 gl_RayFlagsSkipClosestHitShaderEXT,0xffu,o,0.002,d,420.0);rayQueryProceedEXT(q);
 return rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionNoneEXT?1.0:0.0;}
void main(){
 vec3 N=normalize(vNormalWS),L=normalize(-uFrame.lightDir.xyz);
 vec3 V=normalize(uFrame.camPosWS.xyz-vWorldPos);
 float density=biomeFogDensityMult(vBiomeWeights.y,vBiomeWeights.z);
 float fog=(1.0-exp(-max(vViewZ-uFrame.fogParams.y,0.0)*uFrame.fogParams.x*density));
 fog*=exp(-max(vWorldPos.y-uFrame.miscParams.y,0.0)*uFrame.fogParams.w);
 float fogAmt=clamp(fog,0.0,uFrame.fogParams.z);
 float vis=1.0;if(dot(N,L)>0.0&&fogAmt<0.85*uFrame.fogParams.z)
   vis=traceShadow(vWorldPos+N*0.02,L);
 vis=mix(1.0,vis,uFrame.lightParams.y);
 vec3 albedo=texture(uTextures[nonuniformEXT(pc.textureIndex)],vUV).rgb*vColorJitter;
 vec3 gold=vec3(dot(albedo,vec3(0.30,0.59,0.11)))*vec3(1.22,0.86,0.28);
 albedo=mix(albedo,gold,clamp(vBiomeWeights.y*uFrame.stylePaint1.y,0.0,0.72));
 float rawAO=texture(uSSAO,gl_FragCoord.xy/vec2(textureSize(uSSAO,0))).r;
 float root=1.0-clamp(pc.groundOcclusion,0.0,1.0)*
            pow(clamp(1.0-vHeightFrac,0.0,1.0),1.4);
 float ao=rawAO*mix(root,1.0,0.28);
 vec3 irr=textureLod(uEnvMap,N,uFrame.iblParams.x).rgb;
 vec2 sunXZ=normalize(-L.xz+1e-5);
 float directMult=forestCanopyDirect(vWorldPos,vBiomeWeights.y,
   uFrame.miscParams.z,sunXZ)*mountainDirectMultiplier(vBiomeWeights.z);
 vec4 amb=biomeAmbient(vBiomeWeights.x,vBiomeWeights.y,vBiomeWeights.z);
 vec3 color=physicalPaintSurface(albedo,N,V,L,max(pc.roughnessScalar,.72),
   0.0,vis,ao,irr,directMult,amb.w)*amb.rgb;
 color+=foliageTransmission(albedo,N,V,L,vis,
   pc.foliageSssStrength*uFrame.stylePaint1.z);
 vec3 viewDir=normalize(vWorldPos-uFrame.camPosWS.xyz);
 vec3 fogColor=textureLod(uEnvMap,vec3(viewDir.x,max(viewDir.y,0.04),viewDir.z),1.0).rgb;
 fogColor=biomeFogTint(fogColor,vBiomeWeights.y,vBiomeWeights.z);
 color=mix(color,fogColor,fogAmt);
 int dbg=int(uFrame.miscParams.x+0.5);
 if(dbg==1)color=albedo;else if(dbg==2)color=N*0.5+0.5;
 else if(dbg==3)color=vec3(fogAmt);else if(dbg==4)color=vec3(rawAO);
 else if(dbg==5)color=vec3(vis);else if(dbg==7)color=vBiomeWeights;
 else if(dbg==6)color=(any(isnan(color))||any(isinf(color)))?vec3(1,0,1):vec3(0);
 outColor=vec4(max(color,vec3(0)),1);
}
