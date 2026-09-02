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
#include "skyModel.glsl"
#include "biomeLighting.glsl"
#include "paintMaterial.glsl"
#include "fog.glsl"
layout(set=2,binding=0) uniform accelerationStructureEXT uTLAS;
layout(set=3,binding=0) uniform sampler2D uSSAO;
layout(set=4,binding=0) uniform samplerCube uEnvMap;
layout(push_constant) uniform Push {
 mat4 model; uint textureIndex,roughnessIndex,metallicIndex,aoIndex;
 uint normalIndex,opacityIndex;
 float roughnessScalar,metallicScalar,aoScalar,alphaCutoff; uint materialFlags;
 float windStrength,windSpeed,windMeshHeight,groundOcclusion,foliageSssStrength;
} pc;
float traceShadow(vec3 o,vec3 d){rayQueryEXT q;rayQueryInitializeEXT(q,uTLAS,
 gl_RayFlagsTerminateOnFirstHitEXT|gl_RayFlagsOpaqueEXT|
 gl_RayFlagsSkipClosestHitShaderEXT,0xffu,o,0.002,d,420.0);rayQueryProceedEXT(q);
 return rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionNoneEXT?1.0:0.0;}
const uint HAS_ROUGHNESS_MAP=1u<<0;
const uint HAS_METALLIC_MAP=1u<<1;
const uint HAS_AO_MAP=1u<<2;
const uint ROUGHNESS_IS_GLOSS=1u<<3;
const uint HAS_NORMAL_MAP=1u<<10;
const uint HAS_OPACITY_MAP=1u<<11;
float materialChannel(vec4 sampleValue,uint shift){uint channel=(pc.materialFlags>>shift)&3u;
 return channel==0u?sampleValue.r:(channel==1u?sampleValue.g:(channel==2u?sampleValue.b:sampleValue.a));}
void main(){
 vec3 N=normalize(vNormalWS),L=normalize(-uFrame.lightDir.xyz);
 vec3 V=normalize(uFrame.camPosWS.xyz-vWorldPos);
 // Fog first, so the shadow ray below can be skipped where fog has already
 // swallowed the surface. The biome density/tint hooks now feed the ground
 // layer directly instead of scaling a lerp factor.
 vec3 viewDirWS=normalize(vWorldPos-uFrame.camPosWS.xyz);
 vec3 skyAhead=textureLod(uEnvMap,viewDirWS,1.0).rgb;
 vec3 skyAbove=textureLod(uEnvMap,
   vec3(viewDirWS.x,max(viewDirWS.y,0.25),viewDirWS.z),2.0).rgb;
 FogSample fog=fogAlongView(vWorldPos,
   biomeFogDensityMult(vBiomeWeights.y,vBiomeWeights.z),1.0,
   biomeFogTint(vec3(1.0),vBiomeWeights.y,vBiomeWeights.z),skyAhead,skyAbove);
 float vis=1.0;if(dot(N,L)>0.0&&fog.opacity<0.9)
   vis=traceShadow(vWorldPos+N*0.02,L);
 vis=mix(1.0,vis,uFrame.lightParams.y);
 vec4 baseSample=texture(uTextures[nonuniformEXT(pc.textureIndex)],vUV);
 float alpha=baseSample.a;
 if((pc.materialFlags&HAS_OPACITY_MAP)!=0u)alpha*=materialChannel(
   texture(uTextures[nonuniformEXT(pc.opacityIndex)],vUV),12u);
 if(pc.alphaCutoff>0.0&&alpha<pc.alphaCutoff)discard;
 vec3 albedo=baseSample.rgb*vColorJitter;
 if((pc.materialFlags&HAS_NORMAL_MAP)!=0u){vec3 mapN=texture(
   uTextures[nonuniformEXT(pc.normalIndex)],vUV).xyz*2.0-1.0;
   N=paintPerturbNormal(N,vWorldPos,vUV,mapN);}
 vec3 gold=vec3(dot(albedo,vec3(0.30,0.59,0.11)))*vec3(1.22,0.86,0.28);
 albedo=mix(albedo,gold,clamp(vBiomeWeights.y*uFrame.stylePaint1.y,0.0,0.72));
 float rawAO=texture(uSSAO,gl_FragCoord.xy/vec2(textureSize(uSSAO,0))).r;
 float roughness=pc.roughnessScalar;
 if((pc.materialFlags&HAS_ROUGHNESS_MAP)!=0u){roughness=materialChannel(
   texture(uTextures[nonuniformEXT(pc.roughnessIndex)],vUV),4u);
   if((pc.materialFlags&ROUGHNESS_IS_GLOSS)!=0u)roughness=1.0-roughness;}
 float metallic=pc.metallicScalar;
 if((pc.materialFlags&HAS_METALLIC_MAP)!=0u)metallic=materialChannel(
   texture(uTextures[nonuniformEXT(pc.metallicIndex)],vUV),6u);
 float materialAO=pc.aoScalar;
 if((pc.materialFlags&HAS_AO_MAP)!=0u)materialAO=materialChannel(
   texture(uTextures[nonuniformEXT(pc.aoIndex)],vUV),8u);
 float root=1.0-clamp(pc.groundOcclusion,0.0,1.0)*
            pow(clamp(1.0-vHeightFrac,0.0,1.0),1.4);
 float ao=rawAO*materialAO*mix(root,1.0,0.28);
 vec3 irr=textureLod(uEnvMap,N,uFrame.iblParams.x).rgb;
 vec2 sunXZ=normalize(-L.xz+1e-5);
 float directMult=forestCanopyDirect(vWorldPos,vBiomeWeights.y,
   uFrame.miscParams.z,sunXZ)*mountainDirectMultiplier(vBiomeWeights.z);
 vec4 amb=biomeAmbient(vBiomeWeights.x,vBiomeWeights.y,vBiomeWeights.z);
 roughness=max(roughness,.72);
 vec3 color=physicalPaintSurface(albedo,N,V,L,roughness,
   metallic,vis,ao,irr,directMult,amb.w)*amb.rgb;
 vec3 envSpec=textureLod(uEnvMap,reflect(-V,N),roughness*uFrame.iblParams.y).rgb;
 color+=physicalEnvironmentSpecular(albedo,N,V,roughness,metallic,ao,envSpec)*
        uFrame.iblParams.z;
 color+=physicalPointLights(albedo,N,V,vWorldPos,roughness,metallic);
 color+=foliageTransmission(albedo,N,V,L,vis,
   pc.foliageSssStrength*uFrame.stylePaint1.z);
 color=fogApply(color,fog);
 int dbg=int(uFrame.miscParams.x+0.5);
 if(dbg==1)color=albedo;else if(dbg==2)color=N*0.5+0.5;
 else if(dbg==3)color=vec3(fog.opacity);else if(dbg==4)color=vec3(rawAO);
 else if(dbg==5)color=vec3(vis);else if(dbg==7)color=vBiomeWeights;
 else if(dbg==6)color=(any(isnan(color))||any(isinf(color)))?vec3(1,0,1):vec3(0);
 outColor=vec4(max(color,vec3(0)),1);
}
