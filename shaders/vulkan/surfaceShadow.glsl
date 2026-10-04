#ifndef SURFACE_SHADOW_GLSL
#define SURFACE_SHADOW_GLSL

// Shared by terrain, ordinary meshes and instanced foliage. Previously trees
// always used one hard ray while terrain/props silently capped the same public
// sample setting at four, producing incompatible shadows in the same scene.
//
// Includers must enable GL_EXT_buffer_reference and
// GL_EXT_buffer_reference_uvec2 in their own header (extension directives
// cannot follow declarations) and declare uTextures before including this.

// ---- alpha-tested shadow rays ----------------------------------------------
// Alpha-masked meshes (spruce branch cards, leaf clusters, fences) have their
// BLAS built NON-opaque, so traversal hands each hit on them back to us as a
// candidate, and we confirm it only where the material is solid. Without
// this every foliage card casts a solid rectangle of shadow.
//
// "Solid" is read from a per-triangle opacity micromap (256 bits over a
// 16x16 barycentric subdivision) precomputed on the CPU -- see
// buildTriangleMicromap in VulkanRenderer.cpp for why, and for the
// micro-triangle numbering reproduced here.
struct RtAlphaMesh { uvec2 masks; uvec2 reserved; };
layout(buffer_reference, std430, buffer_reference_align=16) readonly buffer RtAlphaMeshes { RtAlphaMesh m[]; };
layout(buffer_reference, std430, buffer_reference_align=4) readonly buffer RtMicromap { uint w[]; };
const uint RT_MICRO_N=16u;
const uint RT_MICRO_WORDS=RT_MICRO_N*RT_MICRO_N/32u;

bool rtAlphaSolid(uint mesh,uint prim,vec2 bary){
    mesh&=0x7fffffu; // high custom-index bit identifies grass instances
    if(uFrame.rtAlphaTable.x==0u&&uFrame.rtAlphaTable.y==0u)return true;
    uvec2 masks=RtAlphaMeshes(uFrame.rtAlphaTable.xy).m[mesh].masks;
    if(masks.x==0u&&masks.y==0u)return true;
    vec2 p=clamp(bary,0.0,1.0)*float(RT_MICRO_N);
    uint j=min(uint(p.y),RT_MICRO_N-1u);
    uint i=min(uint(p.x),RT_MICRO_N-1u-j);
    uint upper=(fract(p.x)+fract(p.y)>1.0&&i+j+2u<=RT_MICRO_N)?1u:0u;
    uint index=2u*RT_MICRO_N*j-j*j+2u*i+upper;
    uint word=RtMicromap(masks).w[prim*RT_MICRO_WORDS+(index>>5u)];
    return (word&(1u<<(index&31u)))!=0u;
}

// Set by traceShadow: whether the committed hit was alpha-masked geometry.
bool gShadowHitFoliage=false;
bool gShadowHitGrass=false;

float traceShadowMasked(vec3 origin,vec3 direction,float maxDistance,uint mask) {
    gShadowHitFoliage=false;
    gShadowHitGrass=false;
    rayQueryEXT query;
    rayQueryInitializeEXT(query,uTLAS,
        gl_RayFlagsTerminateOnFirstHitEXT|gl_RayFlagsSkipClosestHitShaderEXT,
        mask,origin,.002,direction,maxDistance);
    while(rayQueryProceedEXT(query)) {
        // Only non-opaque (alpha-masked) geometry ever lands here.
        if(rayQueryGetIntersectionTypeEXT(query,false)==
           gl_RayQueryCandidateIntersectionTriangleEXT&&
           rtAlphaSolid(rayQueryGetIntersectionInstanceCustomIndexEXT(query,false),
                        rayQueryGetIntersectionPrimitiveIndexEXT(query,false),
                        rayQueryGetIntersectionBarycentricsEXT(query,false)))
            rayQueryConfirmIntersectionEXT(query);
    }
    uint hit=rayQueryGetIntersectionTypeEXT(query,true);
    if(hit==gl_RayQueryCommittedIntersectionNoneEXT)return 1.0;
    uint instance=rayQueryGetIntersectionInstanceCustomIndexEXT(query,true);
    gShadowHitGrass=(instance&0x800000u)!=0u;
    // Opaque geometry is committed without ever being a candidate; only
    // alpha-masked meshes reach rayQueryConfirmIntersectionEXT, so their
    // record is the one with items.
    if(uFrame.rtAlphaTable.x!=0u||uFrame.rtAlphaTable.y!=0u){
        uint mesh=instance&0x7fffffu;
        uvec2 masks=RtAlphaMeshes(uFrame.rtAlphaTable.xy).m[mesh].masks;
        gShadowHitFoliage=masks.x!=0u||masks.y!=0u;
    }
    return 0.0;
}

float traceShadowRange(vec3 origin,vec3 direction,float maxDistance) {
    float visibility=traceShadowMasked(origin,direction,maxDistance,0xffu);
    if(!gShadowHitGrass)return visibility;
    // Thin leaves transmit light: an opaque black line per proxy blade made
    // open meadows look like wire grids. Check the world separately so this
    // fill never erases an opaque tree/rock shadow behind the grass hit.
    return min(.45,traceShadowMasked(origin,direction,maxDistance,0x01u));
}

float traceShadow(vec3 origin,vec3 direction) {
    return traceShadowRange(origin,direction,420.0);
}

float shadowVis(vec3 origin,vec3 L) {
    // Multiplying the result by zero later does not avoid tracing the rays.
    if(uFrame.lightParams.y<=.001)return 1.0;
    float radius=max(uFrame.lightParams.z,0.0);
    int count=clamp(int(uFrame.lightParams.w),1,8);
    // Far surfaces get fewer rays: past ~120 m a penumbra is a pixel or two
    // wide, and those pixels are the ones fog is already eating.
    float dist=length(uFrame.camPosWS.xyz-origin);
    count=min(count,int(mix(8.0,2.0,smoothstep(40.0,160.0,dist))));
    if(radius<.0005||count<=1)return traceShadow(origin,L);
    vec3 T=normalize(cross(L,abs(L.y)<.9?vec3(0,1,0):vec3(1,0,0)));
    vec3 B=cross(L,T);
    // Two probes on opposite rims of the solar disc first. Where they agree
    // the pixel is almost surely fully lit or in umbra -- which is most of
    // the frame -- and the remaining rays would all say the same thing.
    // Only a disagreement (a penumbra) pays for the full stratified set.
    // This matters since foliage became alpha-tested: every ray through a
    // crown now walks many transparent needle cards, and six of them per
    // pixel tripled the frame time.
    float probeA=traceShadow(origin,normalize(L+radius*T));
    bool foliage=gShadowHitFoliage;
    float probeB=traceShadow(origin,normalize(L-radius*T));
    foliage=foliage||gShadowHitFoliage;
    if(probeA==probeB)return probeA;
    // Dappled light under a crown is a scatter of gaps, not a smooth
    // penumbra; more rays through it cost a lot (each walks many cut-out
    // cards) and only average the dapple away.
    if(foliage)return 0.5*(probeA+probeB);
    float visibility=probeA+probeB;
    // Deterministic stratified solar-disc samples. No position hash: that
    // randomized the penumbra at every pixel without temporal accumulation.
    for(int i=0;i<count;++i) {
        float r=radius*sqrt((float(i)+.5)/float(count));
        float angle=float(i)*2.39996323;
        visibility+=traceShadow(origin,normalize(L+r*(T*cos(angle)+B*sin(angle))));
    }
    return visibility/float(count+2);
}
#endif
