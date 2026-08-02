#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D uHdr;
layout(set=0,binding=1) uniform sampler2D uBloom;
layout(set=0,binding=2) uniform sampler2D uBloomWide;
layout(set=0,binding=3) uniform sampler2D uDepth;
#include "frameData.glsl"
layout(push_constant) uniform Push {
 float exposure,gamma,saturation,contrast,vignette;
 int tonemapMode; float bloomIntensity,gradeExposureBias;
 vec4 gradeTint; mat4 invProj;
} pc;
float lum(vec3 c){return dot(c,vec3(.2126,.7152,.0722));}
vec3 aces(vec3 x){const float a=2.51,b=.03,c=2.43,d=.59,e=.14;
 return clamp((x*(a*x+b))/(x*(c*x+d)+e),0.0,1.0);}
vec3 painterly(vec3 c){float y=lum(c);vec3 chroma=c/max(y,.0001);
 float mapped=y/(1.0+y*.72);float white=1.0-exp(-y*.18);
 return chroma*mapped*mix(1.0,.78,white*.35);}
vec3 viewPos(vec2 uv,float d){vec4 p=pc.invProj*vec4(uv*2.0-1.0,d,1);
 return p.xyz/max(p.w,1e-5);}
float edgeMask(vec2 uv){
 ivec2 sz=textureSize(uDepth,0);vec2 px=uFrame.stylePost0.z/vec2(sz);
 float d=texture(uDepth,uv).r;if(d>=1.0)return 0.0;
 float dl=texture(uDepth,uv-vec2(px.x,0)).r,dr=texture(uDepth,uv+vec2(px.x,0)).r;
 float du=texture(uDepth,uv+vec2(0,px.y)).r,dd=texture(uDepth,uv-vec2(0,px.y)).r;
 vec3 p=viewPos(uv,d),pl=viewPos(uv-vec2(px.x,0),dl),pr=viewPos(uv+vec2(px.x,0),dr);
 vec3 pu=viewPos(uv+vec2(0,px.y),du),pd=viewPos(uv-vec2(0,px.y),dd);
 float de=max(max(abs(dl-dr),abs(du-dd)),0.0)/max(fwidth(d),1e-5);
 vec3 n=normalize(cross(pr-pl,pu-pd));float ne=1.0-abs(n.z);
 float e=smoothstep(uFrame.stylePost1.x,uFrame.stylePost1.x*5.0,de*.002);
 e=max(e,smoothstep(uFrame.stylePost1.y,1.0,ne));
 float dist=length(p);e*=1.0-smoothstep(uFrame.stylePost1.z*.65,uFrame.stylePost1.z,dist);
 return clamp(e,0.0,1.0);
}
vec3 filteredHdr(vec2 uv){
 vec2 px=1.0/vec2(textureSize(uHdr,0));vec3 c=texture(uHdr,uv).rgb;
 vec3 n=texture(uHdr,uv+vec2(0,px.y)).rgb,s=texture(uHdr,uv-vec2(0,px.y)).rgb;
 vec3 e=texture(uHdr,uv+vec2(px.x,0)).rgb,w=texture(uHdr,uv-vec2(px.x,0)).rgb;
 float range=max(max(lum(n),lum(s)),max(lum(e),lum(w)))-min(min(lum(n),lum(s)),min(lum(e),lum(w)));
 float blend=smoothstep(.035,.20,range)*.36*uFrame.styleOutlineColor.w;
 return mix(c,(n+s+e+w)*.25,blend);
}
void main(){
 vec3 hdr=filteredHdr(vUV);hdr+=texture(uBloom,vUV).rgb*pc.bloomIntensity;
 hdr+=texture(uBloomWide,vUV).rgb*pc.bloomIntensity*pc.gradeTint.w;
 hdr*=pc.gradeTint.rgb*exp2(pc.gradeExposureBias)*pc.exposure;
 float y=lum(hdr),split=smoothstep(pc.gradeTint.w*.0+.18,.72,y);
 hdr*=mix(uFrame.styleSplitShadow.rgb,uFrame.styleSplitHighlight.rgb,
          smoothstep(uFrame.stylePost0.y-.25,uFrame.stylePost0.y+.25,split));
 float l=lum(hdr);float sat=pc.saturation+uFrame.stylePost0.x;
 hdr=mix(vec3(l),hdr,sat);hdr=(hdr-.18)*pc.contrast+.18;
 float edge=edgeMask(vUV);hdr*=1.0-edge*uFrame.stylePost0.w*.12;
 if(pc.tonemapMode==0)hdr=painterly(max(hdr,vec3(0)));
 else if(pc.tonemapMode==1)hdr=aces(max(hdr,vec3(0)));
 else if(pc.tonemapMode==2)hdr=max(hdr,vec3(0))/(1.0+max(hdr,vec3(0)));
 else hdr=clamp(hdr,0.0,1.0);
 hdr=mix(hdr,uFrame.styleOutlineColor.rgb,edge*uFrame.stylePost0.w);
 float vig=1.0-dot(vUV-.5,vUV-.5)*pc.vignette*1.6;hdr*=clamp(vig,0.0,1.0);
 float grain=fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5453)-.5;
 hdr*=1.0+grain*uFrame.stylePost1.w*(.35+.65*lum(hdr));
 hdr=pow(clamp(hdr,0.0,1.0),vec3(1.0/max(pc.gamma,0.1)));
 outColor=vec4(hdr,1);
}
