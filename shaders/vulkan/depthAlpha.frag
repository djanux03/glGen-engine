#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(location=1) in vec2 vUV;
layout(set=0,binding=0) uniform sampler2D uTextures[];

// Full scene push layout: the instanced vertex shader reads the trailing wind
// fields, while this fragment shader reads only material alpha. Keeping one
// exact declaration prevents the depth and color poses from drifting apart.
layout(push_constant) uniform Push {
    mat4 model;
    uint textureIndex,roughnessIndex,metallicIndex,aoIndex;
    uint normalIndex,opacityIndex;
    float roughnessScalar,metallicScalar,aoScalar,alphaCutoff;
    uint materialFlags;
    float windStrength,windSpeed,windMeshHeight,groundOcclusion;
    float foliageSssStrength;
} pc;

const uint HAS_OPACITY_MAP=1u<<11;

float opacityChannel(vec4 value){
    uint channel=(pc.materialFlags>>12u)&3u;
    return channel==0u?value.r:(channel==1u?value.g:
           (channel==2u?value.b:value.a));
}

void main(){
    float alpha=texture(uTextures[nonuniformEXT(pc.textureIndex)],vUV).a;
    if((pc.materialFlags&HAS_OPACITY_MAP)!=0u)
        // Replace, not multiply -- must match mesh.frag/meshInstanced.frag
        // or the prepass and colour pass disagree about which texels exist.
        alpha=opacityChannel(texture(uTextures[nonuniformEXT(pc.opacityIndex)],vUV));
    if(alpha<pc.alphaCutoff)discard;
}
