#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_ray_query : require
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec3 vNormalWS;
layout(location=1) in vec2 vUV;
layout(location=2) in vec3 vWorldPos;
layout(location=3) in float vViewZ;
layout(location=4) in vec4 vTerrainParams;
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
layout(push_constant) uniform Push { mat4 model; uint textureIndex; } pc;
const uint NO_TEX=0xffffffffu;

float valueNoise(vec2 p){
 vec2 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);
 float a=paintHash13(vec3(i,1)),b=paintHash13(vec3(i+vec2(1,0),1));
 float c=paintHash13(vec3(i+vec2(0,1),1)),d=paintHash13(vec3(i+1.0,1));
 return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
}
float fbm(vec2 p){return valueNoise(p)*0.58+valueNoise(p*2.17+9.2)*0.29+
 valueNoise(p*4.31-5.7)*0.13;}
float traceShadow(vec3 o,vec3 d){rayQueryEXT q;rayQueryInitializeEXT(q,uTLAS,
 gl_RayFlagsTerminateOnFirstHitEXT|gl_RayFlagsOpaqueEXT|
 gl_RayFlagsSkipClosestHitShaderEXT,0xffu,o,0.01,d,420.0);rayQueryProceedEXT(q);
 return rayQueryGetIntersectionTypeEXT(q,true)==gl_RayQueryCommittedIntersectionNoneEXT?1.0:0.0;}
float shadowVis(vec3 o,vec3 L){float r=uFrame.lightParams.z;int n=clamp(int(uFrame.lightParams.w),1,4);
 if(r<=0.0005||n==1)return traceShadow(o,L);vec3 t=normalize(cross(L,abs(L.y)<.9?vec3(0,1,0):vec3(1,0,0)));
 vec3 b=cross(L,t);float s=0;for(int i=0;i<n;i++){float a=paintHash13(vWorldPos+float(i)*13.1)*6.283;
 float rr=sqrt(paintHash13(vWorldPos.zyx+float(i)*27.7))*r;s+=traceShadow(o,normalize(L+(cos(a)*t+sin(a)*b)*rr));}return s/float(n);}
uint layerTex(int i){if(i==0)return uFrame.terrainTexA.x;if(i==1)return uFrame.terrainTexA.w;
 if(i==2)return uFrame.terrainTexB.z;if(i==3)return uFrame.terrainTexC.y;return uFrame.terrainTexD.x;}
uint layerNormalTex(int i){if(i==0)return uFrame.terrainTexA.y;if(i==1)return uFrame.terrainTexB.x;
 if(i==2)return uFrame.terrainTexB.w;if(i==3)return uFrame.terrainTexC.z;return uFrame.terrainTexD.y;}
uint layerRoughTex(int i){if(i==0)return uFrame.terrainTexA.z;if(i==1)return uFrame.terrainTexB.y;
 if(i==2)return uFrame.terrainTexC.x;if(i==3)return uFrame.terrainTexC.w;return uFrame.terrainTexD.z;}
float layerTiling(int i){if(i==0)return uFrame.terrainTiling0.x;if(i==1)return uFrame.terrainTiling0.y;
 if(i==2)return uFrame.terrainTiling0.z;if(i==3)return uFrame.terrainTiling0.w;return uFrame.terrainTiling1.x;}
vec3 paintLayer(int i,vec2 xz){
 float scale=max(uFrame.terrainPaintLit[i].w,0.25);
 float m=fbm(xz/scale)-0.5;
 vec3 c=mix(uFrame.terrainPaintShade[i].rgb,uFrame.terrainPaintLit[i].rgb,
            clamp(0.58+m*0.34,0.0,1.0));
 float strength=uFrame.terrainPaintShade[i].w;uint tex=layerTex(i);
 if(strength>0.001&&tex!=NO_TEX){vec3 ov=texture(uTextures[nonuniformEXT(tex)],
   xz/max(layerTiling(i),0.5)).rgb;float y=max(dot(ov,vec3(.2126,.7152,.0722)),.08);
   c*=mix(vec3(1),ov/y,strength);}
 return c;
}
void main(){
 vec3 smoothN=normalize(vNormalWS);
 float wF=vTerrainParams.z,wM=vTerrainParams.w,wMead=clamp(1.0-wF-wM,0.0,1.0);
 float slope=1.0-clamp(smoothN.y,0.0,1.0);
 float rock=clamp(smoothstep(uFrame.terrainMat2.x,uFrame.terrainMat2.y,slope)*
   mix(.72,1.16,vTerrainParams.y)+wM*.18,0.0,1.0);
 float scree=rock*clamp(wM*1.2,0.0,1.0);
 float dirt=smoothstep(.50,.16,vTerrainParams.x)*(1.0-rock)*uFrame.terrainMat2.z;
 float weights[5];weights[0]=wMead*(1.0-rock)*(1.0-dirt);
 weights[1]=wF*(1.0-rock)*(1.0-dirt);weights[2]=dirt;
 weights[3]=rock-scree;weights[4]=scree;
 float total=max(weights[0]+weights[1]+weights[2]+weights[3]+weights[4],.001);
 vec3 albedo=vec3(0),mappedNormal=vec3(0);float mappedNormalWeight=0.0;
 float mappedRoughness=0.0,mappedRoughnessWeight=0.0;
 for(int i=0;i<5;i++){
   weights[i]/=total;
   vec2 layerUV=vWorldPos.xz/max(layerTiling(i),0.5);
   albedo+=paintLayer(i,vWorldPos.xz)*weights[i];
   uint normalTex=layerNormalTex(i);
   if(normalTex!=NO_TEX){
     vec3 tangentNormal=texture(uTextures[nonuniformEXT(normalTex)],layerUV).xyz*2.0-1.0;
     mappedNormal+=paintPerturbNormal(smoothN,vWorldPos,layerUV,tangentNormal)*weights[i];
     mappedNormalWeight+=weights[i];
   }
   uint roughTex=layerRoughTex(i);
   if(roughTex!=NO_TEX){
     mappedRoughness+=texture(uTextures[nonuniformEXT(roughTex)],layerUV).r*weights[i];
     mappedRoughnessWeight+=weights[i];
   }
 }
 // A broad procedural layer alone collapses to a flat fill at ground-level
 // camera distances. Two restrained, differently oriented frequencies supply
 // aggregate, grit and damp patches without imposing a themed pattern.
 // uFrame.terrainMat2.w = antiTileStrength (default 0.35)
 // uFrame.terrainMat3: x=biomeTintEnabled (dead), y=biomeTintIntensity (dead), z=macroVariationStrength (default 0.15), w=rockDetailStrength (default 0.3)
 float aggregate=fbm(vWorldPos.xz*0.82)-0.5;
 float grit=valueNoise(vWorldPos.xz*5.7+aggregate*2.1)-0.5;
 float damp=smoothstep(0.20,0.48,fbm(vWorldPos.xz*0.115+vec2(17.0,-9.0)));
 albedo*=1.0+(aggregate*1.6+grit*0.5)*uFrame.terrainMat3.z;
 albedo=mix(albedo,albedo*vec3(.68,.73,.77),damp*.22*(1.0-rock));
 vec3 gold=vec3(dot(albedo,vec3(.30,.59,.11)))*vec3(1.18,.82,.27);
 albedo=mix(albedo,gold,wF*uFrame.stylePaint1.y*.55);
 float footprint=length(fwidth(vWorldPos.xz));
 float strokes=(sin(vWorldPos.x*21.0+valueNoise(vWorldPos.xz*2.0)*5.0)*.5+.5);
 strokes=smoothstep(.72,.92,strokes)*(1.0-smoothstep(.02,.22,footprint));
 albedo*=1.0+strokes*(weights[0]+weights[1])*(uFrame.terrainMat2.w*0.2857);
 float edge=0.0;for(int i=0;i<5;i++)edge+=length(vec2(dFdx(weights[i]),dFdy(weights[i])));
 albedo*=1.0-clamp(edge*uFrame.stylePaint0.w,0.0,.25);
 float paper=valueNoise(vWorldPos.xz*2.0)-.5;
 albedo*=1.0+paper*uFrame.stylePaint0.z;
 vec3 facet=normalize(cross(dFdx(vWorldPos),dFdy(vWorldPos)));
 if(dot(facet,smoothN)<0.0)facet=-facet;
 vec3 N=normalize(mix(smoothN,facet,clamp(wM*rock*uFrame.stylePaint1.x*(uFrame.terrainMat3.w/0.3),0.0,1.0)));
 if(mappedNormalWeight>0.001){
   vec3 detailNormal=normalize(mappedNormal/max(mappedNormalWeight,0.001));
   N=normalize(mix(N,detailNormal,clamp(mappedNormalWeight*.82,0.0,.82)));
 }
 vec3 L=normalize(-uFrame.lightDir.xyz),V=normalize(uFrame.camPosWS.xyz-vWorldPos);
 // Fog before the shadow rays, so fully fogged terrain skips them entirely.
 vec3 viewDirWS=normalize(vWorldPos-uFrame.camPosWS.xyz);
 vec3 skyAhead=textureLod(uEnvMap,viewDirWS,1.0).rgb;
 vec3 skyAbove=textureLod(uEnvMap,vec3(viewDirWS.x,max(viewDirWS.y,.25),viewDirWS.z),2.0).rgb;
 FogSample fog=fogAlongView(vWorldPos,biomeFogDensityMult(wF,wM),
   mountainAerialPerspective(max(uFrame.camPosWS.y-vWorldPos.y,0.0),wM),
   biomeFogTint(vec3(1.0),wF,wM),skyAhead,skyAbove);
 float vis=1.0;if(dot(N,L)>0.0&&fog.opacity<.9)vis=shadowVis(vWorldPos+smoothN*.02,L);
 vis=mix(1.0,vis,uFrame.lightParams.y);
 float ssao=texture(uSSAO,gl_FragCoord.xy/vec2(textureSize(uSSAO,0))).r;
 vec3 irr=textureLod(uEnvMap,N,uFrame.iblParams.x).rgb;
 vec2 sunXZ=normalize(-L.xz+1e-5);float directMult=forestCanopyDirect(vWorldPos,wF,uFrame.miscParams.z,sunXZ)*mountainDirectMultiplier(wM);
 vec4 amb=biomeAmbient(wMead,wF,wM);
 float terrainRoughness=clamp(.91-grit*.10-damp*.18-rock*.16,.48,1.0);
 if(mappedRoughnessWeight>0.001)
   terrainRoughness=mix(terrainRoughness,
     mappedRoughness/max(mappedRoughnessWeight,0.001),
     clamp(mappedRoughnessWeight,0.0,1.0));
 vec3 color=physicalPaintSurface(albedo,N,V,L,terrainRoughness,0.0,vis,ssao,irr,directMult,amb.w)*amb.rgb;
 vec3 envSpec=textureLod(uEnvMap,reflect(-V,N),terrainRoughness*uFrame.iblParams.y).rgb;
 color+=physicalEnvironmentSpecular(albedo,N,V,terrainRoughness,0.0,ssao,envSpec)*
        uFrame.iblParams.z*.55;
 color+=physicalPointLights(albedo,N,V,vWorldPos,terrainRoughness,0.0);
 color=fogApply(color,fog);
 int dbg=int(uFrame.miscParams.x+.5);if(dbg==1)color=albedo;else if(dbg==2)color=N*.5+.5;
 else if(dbg==3)color=vec3(fog.opacity);else if(dbg==4)color=vec3(ssao);else if(dbg==5)color=vec3(vis);
 else if(dbg==7)color=vec3(wMead,wF,wM);else if(dbg==6)color=(any(isnan(color))||any(isinf(color)))?vec3(1,0,1):vec3(0);
 outColor=vec4(max(color,vec3(0)),1);
}
