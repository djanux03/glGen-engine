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
layout(location=4) in vec3 vColorJitter;
layout(location=5) in vec3 vBiomeWeights;
layout(location=6) in float vHeightFrac;
layout(location=7) in vec4 vInstanceBase;
layout(location=8) in vec3 vShadowPos;
layout(location=0) out vec4 outColor;
// Depth is resolved by the Z-prepass (which alpha-tests with depthAlpha.frag)
// and this pass never writes it, so the depth test can safely run BEFORE the
// shader despite the discard below. Without this, `discard` disables early-Z
// and every overlapping foliage-card fragment ran full shading -- fog, soft
// RT shadows, IBL -- before being rejected: dense spruce crowns cost ~40 ms.
layout(early_fragment_tests) in;
layout(set=0,binding=0) uniform sampler2D uTextures[];
#include "frameData.glsl"
#include "skyModel.glsl"
#include "biomeLighting.glsl"
#include "fog.glsl"
layout(set=2,binding=0) uniform accelerationStructureEXT uTLAS;
layout(set=3,binding=0) uniform sampler2D uSSAO;
layout(set=4,binding=0) uniform samplerCube uEnvMap;
#include "cloudShadow.glsl"
layout(push_constant) uniform Push {
 mat4 model; uint textureIndex,roughnessIndex,metallicIndex,aoIndex;
 uint normalIndex,opacityIndex;
 float roughnessScalar,metallicScalar,aoScalar,alphaCutoff; uint materialFlags;
 float windStrength,windSpeed,windMeshHeight,groundOcclusion,foliageSssStrength;
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
float materialChannel(vec4 sampleValue,uint shift){uint channel=(pc.materialFlags>>shift)&3u;
 return channel==0u?sampleValue.r:(channel==1u?sampleValue.g:(channel==2u?sampleValue.b:sampleValue.a));}
void main(){
 // Alpha test before fog or shadow-ray work (normal derivatives precede it): a needle card is
 // ~85% transparent texels, and each one used to trace the full soft-shadow
 // ray set before being thrown away.
 vec4 baseSample=texture(uTextures[nonuniformEXT(pc.textureIndex)],vUV);
 float alpha=baseSample.a;
 // Opacity map replaces base alpha (it IS base alpha for glTF MASK
 // materials; multiplying squared it -- see mesh.frag).
 if((pc.materialFlags&HAS_OPACITY_MAP)!=0u)alpha=materialChannel(
   texture(uTextures[nonuniformEXT(pc.opacityIndex)],vUV),12u);
 // Foliage cards are drawn without culling; seen from behind, the
 // interpolated normal faces away from the viewer and the card shades as
 // if lit from the wrong side. Flip it -- the classic two-sided rule.
 vec3 N=normalize(gl_FrontFacing?vNormalWS:-vNormalWS),L=normalize(-uFrame.lightDir.xyz);
 if((pc.materialFlags&HAS_NORMAL_MAP)!=0u){vec3 mapN=texture(
   uTextures[nonuniformEXT(pc.normalIndex)],vUV).xyz*2.0-1.0;
   // Folded scanned grass already has curved geometry. Full-strength atlas
   // ribs sharpened it into alternating bright/dark pixels at walking range.
   if((pc.materialFlags&(1u<<15))!=0u&&uFrame.terrainTiling1.y>.5)
     mapN=normalize(vec3(mapN.xy*.35,mapN.z));
   N=paintPerturbNormal(N,vWorldPos,vUV,mapN);}
 float normalVariance=paintNormalVariance(N);
 if(pc.alphaCutoff>0.0&&alpha<pc.alphaCutoff)discard;
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
 // Two solar-disc samples soften thin blade shadows without paying for the
 // full tree penumbra integral per grass pixel. Disabled shadows skip ray work.
 bool grass=(pc.materialFlags&(1u<<15))!=0u;
 vec3 albedo=baseSample.rgb*vColorJitter;
 // Cutout scans preserve leaf-local midribs and root/tip value changes, but
 // their dry yellow chroma became fluorescent under meadow transmission.
 // Pull chroma toward a muted olive while retaining the scanned gradients.
 // Woodland's new photographed leaves already contain green, spent yellow
 // and brown tips. The older atlas correction erased that colour variation.
 if(grass&&pc.alphaCutoff>0.0&&uFrame.terrainTiling1.y<.5){
   float leafValue=dot(albedo,vec3(.2126,.7152,.0722));
   albedo=mix(albedo,leafValue*vec3(.73,1.0,.49),.82)*.85;
 }
 float leaf=smoothstep(0.004,0.05,albedo.g-max(albedo.r,albedo.b)*0.92)*
            step(0.001,pc.foliageSssStrength);
 // Backlit leaves need visibility too; otherwise transmission glows through
 // opaque trunks and hills whenever the view-facing normal points away.
 float vis=1.0;if(uFrame.lightParams.y>0.001&&(dot(N,L)>0.0||leaf>.001)&&fog.opacity<0.9){
   // The former 2cm bias jumped across whole grass blades, erasing contact
   // shadows once grass entered the TLAS. Keep the usual bias on large meshes.
   float bias=grass?.004:.02;
   // Starting a ray on the wind-displaced surface while the BLAS retained
   // the rest blade made that blade intersect its own ray in dark stripes.
   vec3 origin=(grass?vShadowPos:vWorldPos)+N*(dot(N,L)>=0.0?bias:-bias);
   if(grass&&uFrame.lightParams.z>.0005&&uFrame.lightParams.w>1.0){
     vec3 T=normalize(cross(L,abs(L.y)<.9?vec3(0,1,0):vec3(1,0,0)));
     float radius=uFrame.lightParams.z*.7;
     vis=.5*(traceShadow(origin,normalize(L+T*radius))+
             traceShadow(origin,normalize(L-T*radius)));
   }else vis=grass?traceShadow(origin,L):shadowVis(origin,L);
 }
 vis=mix(1.0,vis,uFrame.lightParams.y);
 vis*=cloudTransmission(vWorldPos);
 // Canopy volume normals. A tree crown is a cloud of leaves, and light
 // falls off across it as across a rough ellipsoid -- not facet by facet.
 // Scattered crowns here are low-poly shells, so their true normals make
 // every face a separate flat tone and the tree reads as a paper lantern.
 // Bending the normal toward an ellipsoid fitted to the instance gives the
 // soft terminator real foliage has. Masked to green (leaf) texels so the
 // trunk and any rock sharing this layer keep their geometric normals.
 // A grass patch is not a tree crown: fitting its blades to an ellipsoid
 // made an entire bank face the same direction and erased blade highlights.
 float soften=grass?0.0:uFrame.realismParams.y*leaf;
 if(soften>0.001){
   float H=max(vInstanceBase.w,0.05);
   vec3 centre=vInstanceBase.xyz+vec3(0.0,H*0.55,0.0);
   vec3 d=vWorldPos-centre;
   // Crown semi-axes ~0.3H across, 0.45H tall; dividing by the squared
   // axes gives the ellipsoid gradient, i.e. its surface normal.
   vec3 ell=normalize(d/vec3(0.09,0.2025,0.09)/(H*H)+vec3(0.0,1e-4,0.0));
   N=normalize(mix(N,ell,soften));
 }
 vec3 gold=vec3(dot(albedo,vec3(0.30,0.59,0.11)))*vec3(1.22,0.86,0.28);
 albedo=mix(albedo,gold,clamp(vBiomeWeights.y*uFrame.stylePaint1.y,0.0,0.72));
 float rawAO=texture(uSSAO,gl_FragCoord.xy/vec2(textureSize(uSSAO,0))).r;
 // Thin overlapping blades need stronger local occlusion than broad props.
 // Squaring visibility deepens real contacts without tinting exposed tips.
 if(grass)rawAO*=rawAO;
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
 vec2 sunXZ=normalize(-L.xz+1e-5);
 float directMult=forestCanopyDirect(vWorldPos,vBiomeWeights.y,
   uFrame.miscParams.z,sunXZ)*mountainDirectMultiplier(vBiomeWeights.z);
 vec4 amb=biomeAmbient(vBiomeWeights.x,vBiomeWeights.y,vBiomeWeights.z);
 // Roots sit inside the clump's occluded volume. Confine this to the lower
 // quarter of a blade, so its bright tips don't become a uniform dark tint.
 float rootContact=grass?(1.0-smoothstep(.015,.24,vHeightFrac))*
   clamp(pc.groundOcclusion*2.0,0.0,1.0):0.0;
 directMult*=1.0-rootContact*.50;
 // Respect the material's cuticle/bark/stone response. The old blanket .72
 // floor discarded authored gloss and made every scattered surface chalky.
 roughness=clamp(roughness,.08,1.0);
 // Dry meadow leaves have a broad cuticle response. Grazing reflections on
 // the imported scans otherwise made every leaf edge a white specular wire.
 if(grass&&uFrame.terrainTiling1.y>.5)roughness=max(roughness,.82);
 float snow=applySnow(vWorldPos,normalize(vNormalWS),
   (pc.materialFlags & (1u<<14))!=0u ? .72 : 0.0,albedo,N,roughness);
 metallic*=1.0-snow;
 roughness=paintFilterRoughness(roughness,normalVariance);
 // A blade's rough, diffuse response cannot resolve an eight-direction
 // irradiance integral. Coarse sky mips preserve its sky/ground fill with
 // two taps; trees keep the full angular response.
 vec3 irr=uFrame.skyLutParams.x>.5?skyDiffuseIrradiance(N):grass?mix(textureLod(uEnvMap,N,uFrame.iblParams.x).rgb,
                    textureLod(uEnvMap,vec3(0,1,0),uFrame.iblParams.x).rgb,.60)
               :paintDiffuseEnvironment(uEnvMap,N);
 irr*=1.0-rootContact*.75;
 // A narrow, folded leaf represents a bundle of curved surfaces below a
 // pixel. Woodland grass averages its diffuse normal slightly toward the
 // sky; treating each fold as a hard plane made green leaves black wires.
 // Keep the detailed normal for the reflection and transmission below.
 vec3 diffuseN=(grass&&uFrame.terrainTiling1.y>.5)
     ?normalize(mix(N,vec3(0,1,0),.32)):N;
 vec3 color=physicalPaintSurface(albedo,diffuseN,V,L,roughness,
   metallic,vis,ao,irr,directMult,amb.w)*amb.rgb;
 // Thin curved blades scatter some sun across the terminator. A hard
 // one-sided Lambert cutoff made half the meadow read as black wire. The
 // fill is visibility-gated so blades stay dark under real grass/tree shadows.
 if(grass)color+=albedo*uFrame.sunRadiance.rgb*max(.35-max(dot(N,L),0.0),0.0)*
                .40*vis*directMult*ao;
 vec3 R=reflect(-V,N);
 vec3 envSpec=textureLod(uEnvMap,R,roughness*uFrame.iblParams.y).rgb*
              paintHorizonOcclusion(R,normalize(vNormalWS));
 // A full grazing sky reflection erased the leaf's colour into white wire.
 // Dry woodland grass retains a small cuticle reflection and a matte body.
 color+=physicalEnvironmentSpecular(albedo,N,V,roughness,metallic,ao,envSpec)*
        uFrame.iblParams.z*((grass&&uFrame.terrainTiling1.y>.5)?.18:1.0);
 color+=physicalPointLights(albedo,N,V,vWorldPos,roughness,metallic);
 color+=foliageTransmission(albedo,N,V,L,vis,
   pc.foliageSssStrength*leaf*uFrame.stylePaint1.z*(1.0-snow)*(1.0-rootContact*.8));
 // Camera transport is applied after opaque shading.
 int dbg=int(uFrame.miscParams.x+0.5);
 if(dbg==1)color=albedo;else if(dbg==2)color=N*0.5+0.5;
 else if(dbg==3)color=vec3(fog.opacity);else if(dbg==4)color=vec3(rawAO);
 else if(dbg==5)color=vec3(vis);else if(dbg==7)color=vBiomeWeights;
 else if(dbg==6)color=(any(isnan(color))||any(isinf(color)))?vec3(1,0,1):vec3(0);
 // Opaque HDR alpha is metadata: negative marks grass for the post-AA pass.
 // Use the material flag rather than colour so dry/snowy blades still blur.
 outColor=vec4(max(color,vec3(0)),grass?-1.0:1.0);
}
