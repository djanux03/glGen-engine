#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(location=0) in vec3 vNormalWS;
layout(location=1) in vec2 vUV;
layout(location=2) in vec3 vWorldPos;
layout(location=3) in float vViewZ;
layout(location=0) out vec4 outColor;
// Safe despite the alpha discard: the Z-prepass owns depth and this pass
// does not write it. Keeps early-Z for cut-out props (see meshInstanced.frag).
layout(early_fragment_tests) in;
layout(set=0,binding=0) uniform sampler2D uTextures[];
#include "frameData.glsl"
#include "skyModel.glsl"
#include "fog.glsl"
layout(set=2,binding=0) uniform accelerationStructureEXT uTLAS;
layout(set=3,binding=0) uniform sampler2D uSSAO;
layout(set=4,binding=0) uniform samplerCube uEnvMap;
#include "cloudShadow.glsl"

layout(push_constant) uniform Push {
    mat4 model;
    uint textureIndex, roughnessIndex, metallicIndex, aoIndex;
    uint normalIndex,opacityIndex;
    float roughnessScalar, metallicScalar, aoScalar,alphaCutoff;
    uint materialFlags;
    float windStrength,windSpeed,windMeshHeight,groundOcclusion;
    float foliageSssStrength;
} pc;

#include "surfaceShadow.glsl"
#include "paintMaterial.glsl"
#include "snowMaterial.glsl"

const uint HAS_ROUGHNESS_MAP=1u<<0;
const uint HAS_METALLIC_MAP=1u<<1;
const uint HAS_AO_MAP=1u<<2;
const uint ROUGHNESS_IS_GLOSS=1u<<3;
const uint HAS_NORMAL_MAP=1u<<10;
const uint HAS_OPACITY_MAP=1u<<11;
float materialChannel(vec4 sampleValue,uint shift){
    uint channel=(pc.materialFlags>>shift)&3u;
    return channel==0u?sampleValue.r:(channel==1u?sampleValue.g:
           (channel==2u?sampleValue.b:sampleValue.a));
}

void main() {
    // Alpha test before fog and shadow rays (see meshInstanced.frag).
    vec4 baseSample=texture(uTextures[nonuniformEXT(pc.textureIndex)],vUV);
    float alpha=baseSample.a;
    // The opacity map REPLACES base alpha. For glTF MASK/BLEND materials the
    // parser registers the base-colour image again as the opacity source
    // (its alpha channel), so multiplying squared the alpha: a soft 0.7
    // needle edge became 0.49 and failed a 0.5 cutoff, and whole spruce
    // branch sprays vanished while their RT shadows (which read the
    // opacity map alone) stayed.
    if((pc.materialFlags&HAS_OPACITY_MAP)!=0u)
        alpha=materialChannel(texture(uTextures[nonuniformEXT(pc.opacityIndex)],vUV),12u);
    bool foreground=(pc.materialFlags & (1u<<14))!=0u;
    vec3 N=normalize(vNormalWS), L=normalize(-uFrame.lightDir.xyz);
    if((pc.materialFlags&HAS_NORMAL_MAP)!=0u){
        vec3 mapN=texture(uTextures[nonuniformEXT(pc.normalIndex)],vUV).xyz*2.0-1.0;
        N=paintPerturbNormal(N,vWorldPos,vUV,mapN);
    }
    float normalVariance=paintNormalVariance(N);
    // Derivatives must precede discard, including along leaf/needle gaps.
    if(pc.alphaCutoff>0.0&&alpha<pc.alphaCutoff)discard;
    vec3 V=normalize(uFrame.camPosWS.xyz-vWorldPos);
    // Fog is resolved before shading so the shadow rays below can be skipped
    // on surfaces the fog has already swallowed. skyAhead is the true view
    // direction (the aerial layer converges on it); skyAbove is biased up and
    // one mip blurrier, since mist is lit from the sky above it no matter
    // which way the camera looks through it.
    vec3 viewDirWS=normalize(vWorldPos-uFrame.camPosWS.xyz);
    vec3 skyAhead=textureLod(uEnvMap,viewDirWS,1.0).rgb;
    vec3 skyAbove=textureLod(uEnvMap,
        vec3(viewDirWS.x,max(viewDirWS.y,0.25),viewDirWS.z),2.0).rgb;
    FogSample fog=fogAlongView(vWorldPos,1.0,1.0,vec3(1.0),skyAhead,skyAbove);
    float vis=1.0;
    if(!foreground&&dot(N,L)>0.0&&fog.opacity<0.9)
        vis=shadowVis(vWorldPos+N*0.02,L);
    vis=mix(1.0,vis,uFrame.lightParams.y);
    vis*=cloudTransmission(vWorldPos);
    vec3 albedo=baseSample.rgb;
    float roughness=pc.roughnessScalar;
    if((pc.materialFlags&HAS_ROUGHNESS_MAP)!=0u){
        roughness=materialChannel(texture(uTextures[nonuniformEXT(pc.roughnessIndex)],vUV),4u);
        if((pc.materialFlags&ROUGHNESS_IS_GLOSS)!=0u)roughness=1.0-roughness;
    }
    float metallic=pc.metallicScalar;
    if((pc.materialFlags&HAS_METALLIC_MAP)!=0u)
        metallic=materialChannel(texture(uTextures[nonuniformEXT(pc.metallicIndex)],vUV),6u);
    float materialAO=pc.aoScalar;
    if((pc.materialFlags&HAS_AO_MAP)!=0u)
        materialAO=materialChannel(texture(uTextures[nonuniformEXT(pc.aoIndex)],vUV),8u);
    float ssao=texture(uSSAO,gl_FragCoord.xy/vec2(textureSize(uSSAO,0))).r;
    if(foreground)ssao=1.0;
    float ao=clamp(ssao*materialAO,0.0,1.0);

    float snow=foreground?0.0:applySnow(vWorldPos,normalize(vNormalWS),1.0,albedo,N,roughness);
    metallic*=(1.0-snow);
    roughness=paintFilterRoughness(roughness,normalVariance);

    vec3 irr=paintDiffuseEnvironment(uEnvMap,N);
    vec3 reflection=reflect(-V,N);
    vec3 envSpec=textureLod(uEnvMap,reflection,
                            roughness*uFrame.iblParams.y).rgb*
                 paintHorizonOcclusion(reflection,normalize(vNormalWS));
    vec3 color=physicalPaintSurface(albedo,N,V,L,roughness,
                                    metallic,vis,ao,irr,1.0,1.0);
    color+=physicalEnvironmentSpecular(albedo,N,V,roughness,metallic,ao,envSpec)*
           uFrame.iblParams.z;
    color+=physicalPointLights(albedo,N,V,vWorldPos,roughness,metallic);
    // These regular-mesh lanes contain emissive RGB/strength; instanced
    // foliage uses the same 128-byte tail for wind instead.
    color+=max(vec3(pc.windStrength,pc.windSpeed,pc.windMeshHeight),vec3(0))*
           max(pc.groundOcclusion,0.0);
    // Camera transport is applied after opaque shading.
    int dbg=int(uFrame.miscParams.x+0.5);
    if(dbg==1) color=albedo; else if(dbg==2) color=N*0.5+0.5;
    else if(dbg==3) color=vec3(fog.opacity); else if(dbg==4) color=vec3(ssao);
    else if(dbg==5) color=vec3(vis); else if(dbg==6)
      color=(any(isnan(color))||any(isinf(color)))?vec3(1,0,1):vec3(0);
    outColor=vec4(max(color,vec3(0)),1);
}
