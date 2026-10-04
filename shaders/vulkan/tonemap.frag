#version 450
#extension GL_GOOGLE_include_directive : require
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D uHdr;
layout(set=0,binding=1) uniform sampler2D uBloom;
// Six-level normalized bloom is supplied in binding 1.
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
// AgX (Troy Sobotka's display transform, as used by Blender 4), via Benjamin
// Wrensch's polynomial fit of the default sigmoid. Scene-linear Rec.709 in,
// LINEAR display values out -- the gamma step below re-encodes them, so the
// inverse inset here decodes the curve's own 2.2 encoding first.
//
// Why AgX over the ACES fit already available as mode 1: ACES tonemaps each
// channel through the same curve, so a bright saturated source keeps its
// ratio between channels until one clips -- a low sun turns a sky patch
// cyan-then-white with a visible hue kink, foliage in hard sun goes neon.
// AgX works in an inset (desaturated) primaries space and then un-insets,
// so highlights converge on white smoothly, which is what film and camera
// sensors do and is most of what reads as "photographic".
vec3 agxContrast(vec3 x){
 vec3 x2=x*x,x4=x2*x2;
 return 15.5*x4*x2-40.14*x4*x+31.96*x4-6.868*x2*x+0.4298*x2+0.1191*x-0.00232;
}
vec3 agx(vec3 c){
 const mat3 inset=mat3(0.842479062253094,0.0423282422610123,0.0423756549057051,
                       0.0784335999999992,0.878468636469772,0.0784336,
                       0.0792237451477643,0.0791661274605434,0.879142973793104);
 const mat3 outset=mat3(1.19687900512017,-0.0528968517574562,-0.0529716355144438,
                        -0.0980208811401368,1.15190312990417,-0.0980434501171241,
                        -0.0990297440797205,-0.0989611768448433,1.15107367264116);
 const float minEv=-12.47393,maxEv=4.026069;
 vec3 v=inset*max(c,vec3(1e-10));
 v=clamp(log2(v),minEv,maxEv);
 v=agxContrast((v-minEv)/(maxEv-minEv));
 // Mild "punchy" look: base AgX is deliberately flat so a grade has room;
 // a slight power and saturation restores the contrast of a real exposure.
 float y=dot(v,vec3(.2126,.7152,.0722));
 v=pow(max(v,vec3(0)),vec3(1.12));
 y=dot(v,vec3(.2126,.7152,.0722));
 v=y+(v-y)*1.12;
 v=outset*v;
 return pow(clamp(v,0.0,1.0),vec3(2.2));
}
vec3 painterly(vec3 c){
 // Bounded luminance shoulder, then compress only out-of-gamut chroma.
 // The old curve approached 1.39 and clipped channels independently: warm
 // clouds became yellow cutouts and bright snow lost all surface shape.
 float y=lum(c),mapped=y/(1.0+y);
 vec3 color=c*(mapped/max(y,.00001));
 float peak=max(color.r,max(color.g,color.b));
 float chromaScale=min(1.0,(1.0-mapped)/max(peak-mapped,.00001));
 return mix(vec3(mapped),color,chromaScale);
}
vec3 viewPos(vec2 uv,float d){vec4 p=pc.invProj*vec4(uv*2.0-1.0,d,1);
 return p.xyz/max(p.w,1e-5);}
float edgeMask(vec2 uv){
 ivec2 sz=textureSize(uDepth,0);vec2 px=uFrame.stylePost0.z/vec2(sz);
 float d=texture(uDepth,uv).r;if(d>=1.0)return 0.0;
 float dl=texture(uDepth,uv-vec2(px.x,0)).r,dr=texture(uDepth,uv+vec2(px.x,0)).r;
 float du=texture(uDepth,uv+vec2(0,px.y)).r,dd=texture(uDepth,uv-vec2(0,px.y)).r;
 vec3 p=viewPos(uv,d),pl=viewPos(uv-vec2(px.x,0),dl),pr=viewPos(uv+vec2(px.x,0),dr);
 vec3 pu=viewPos(uv+vec2(0,px.y),du),pd=viewPos(uv-vec2(0,px.y),dd);
 // A grazing normal is not an edge: the old 1-abs(n.z) test painted
 // entire hillsides and tree faces with outline ink. Compare opposing
 // depth slopes instead; a continuous plane cancels, a silhouette does not.
 float de=max(abs(pl.z+pr.z-2.0*p.z),abs(pu.z+pd.z-2.0*p.z)) /
          max(abs(p.z),1.0);
 float threshold=max(uFrame.stylePost1.x,.0001);
 float e=smoothstep(threshold,threshold*5.0,de);
 // Compare adjacent reconstructed normals for a crease, rather than comparing
 // one normal with the camera. A slanted flat face still produces zero ink.
 vec3 nl=cross(p-pl,pu-p),nr=cross(pr-p,p-pd);
 float lengths=dot(nl,nl)*dot(nr,nr);
 if(lengths>1e-16){
   float crease=1.0-clamp(dot(nl,nr)*inversesqrt(lengths),0.0,1.0);
   e=max(e,smoothstep(max(uFrame.stylePost1.y,.001),1.0,crease));
 }
 float dist=length(p);e*=1.0-smoothstep(uFrame.stylePost1.z*.65,uFrame.stylePost1.z,dist);
 return clamp(e,0.0,1.0);
}
float frostHash(vec2 p){
 return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float frostNoise(vec2 p){
 vec2 i = floor(p), f = fract(p);
 f = f * f * (3.0 - 2.0 * f);
 return mix(mix(frostHash(i), frostHash(i + vec2(1,0)), f.x),
            mix(frostHash(i + vec2(0,1)), frostHash(i + vec2(1,1)), f.x), f.y);
}
float frostPattern(vec2 uv){
 vec2 p = uv * 16.0;
 float f1 = frostNoise(p);
 float f2 = frostNoise(p * 2.8 + vec2(f1 * 1.8));
 float f3 = frostNoise(p * 5.5 - vec2(f2 * 2.5));
 return f1 * 0.5 + f2 * 0.35 + f3 * 0.15;
}

vec3 filteredHdr(vec2 uv){
 vec3 original=texture(uHdr,uv).rgb,centre=original;
 vec2 px=1.0/vec2(textureSize(uHdr,0));
 float softness=uFrame.postAAParams.x;
 bool fxaa=uFrame.styleOutlineColor.w>=.5;
 vec3 n=centre,s=centre,e=centre,w=centre;
 if(uFrame.tldStyleParams.w>.001||softness>.001){
   n=texture(uHdr,uv+vec2(0,-px.y)).rgb;
   s=texture(uHdr,uv+vec2(0, px.y)).rgb;
   e=texture(uHdr,uv+vec2(px.x,0)).rgb;
   w=texture(uHdr,uv-vec2(px.x,0)).rgb;
   vec3 low=min(centre,min(min(n,s),min(e,w))),high=max(centre,max(max(n,s),max(e,w)));
   // The old early return here skipped FXAA whenever TAA detail recovery was
   // enabled, leaving wind-driven grass/leaf edges sharp after history rejection.
   centre=clamp(centre+(centre-(n+s+e+w)*.25)*uFrame.tldStyleParams.w,low,high);
 }
 if(!fxaa&&softness<=.001)return centre;
 // FXAA in HDR space. It follows the actual edge direction, preserving
 // material detail while stabilising one-pixel rebar, branches and rooflines.
 vec3 nw=texture(uHdr,uv+vec2(-1,-1)*px).rgb;
 vec3 ne=texture(uHdr,uv+vec2( 1,-1)*px).rgb;
 vec3 sw=texture(uHdr,uv+vec2(-1, 1)*px).rgb;
 vec3 se=texture(uHdr,uv+vec2( 1, 1)*px).rgb;
 vec3 c=centre;
 float lnw=lum(nw),lne=lum(ne),lsw=lum(sw),lse=lum(se),lc=lum(c);
 float lmin=min(lc,min(min(lnw,lne),min(lsw,lse)));
 float lmax=max(lc,max(max(lnw,lne),max(lsw,lse)));
 // Flat regions need no edge filter. Besides saving taps, this prevents
 // softened texture detail when there is no visible edge to antialias.
 if(lmax-lmin<max(.015,lmax*.10))return c;
 // Use metres, not device Z (which puts almost all of the scene near 1).
 // The nearest depth in the filter footprint makes BOTH sides of a nearby
 // grass/leaf silhouette use its distance, rather than blurring the sky side
 // as if it were far away. Keep the footprint one pixel to avoid broad halos.
 bool surface=false;
 bool grass=false;
 float distanceBlend=0.0;
 if(softness>.001){
   float nearestDepth=1.0;
   ivec2 size=textureSize(uDepth,0),base=ivec2(uv*vec2(size));
   for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
     ivec2 tap=clamp(base+ivec2(x,y),ivec2(0),size-1);
     float d=texelFetch(uDepth,tap,0).r;
     // Follow the same nearest surface as distance filtering. A grass blade
     // behind a closer leaf/rock must not give that foreground object its blur.
     if(d<nearestDepth){
       nearestDepth=d;
       grass=texelFetch(uHdr,tap,0).a<0.0;
     }
   }
   // A visible blade must keep its blur even when an adjacent ground/leaf
   // pixel is closer. Neighbour tags only extend treatment around its outline.
   grass=grass||texelFetch(uHdr,clamp(base,ivec2(0),size-1),0).a<0.0;
   surface=nearestDepth<1.0;
   if(surface)distanceBlend=smoothstep(5.0,100.0,length(viewPos(uv,nearestDepth)));
   // Grass keeps at least the full soft edge treatment even at walking range.
   // Other materials still sharpen nearby and soften gradually with distance.
   softness*=surface?max(grass?1.0:0.0,mix(.20,1.20,distanceBlend)):0.0;
 }
 if(fxaa){
   vec2 dir=vec2(-((lnw+lne)-(lsw+lse)),(lnw+lsw)-(lne+lse));
   float reduce=max((lnw+lne+lsw+lse)*.0078125,.0009765625);
   float invMin=1.0/(min(abs(dir.x),abs(dir.y))+reduce);
   dir=clamp(dir*invMin,vec2(-8),vec2(8))*px;
   vec3 a=.5*(texture(uHdr,uv+dir*(1.0/3.0-.5)).rgb+
              texture(uHdr,uv+dir*(2.0/3.0-.5)).rgb);
   vec3 b=a*.5+.25*(texture(uHdr,uv+dir*-.5).rgb+
                    texture(uHdr,uv+dir*.5).rgb);
   float lb=lum(b);
   vec3 antialiased=(lb<lmin||lb>lmax)?a:b;
   // Retain some spatial AA nearby, but let close stems and surface detail
   // stay sharper. Distant silhouettes get the full filter plus softening.
   c=mix(c,antialiased,surface&&!grass?mix(.35,1.0,distanceBlend):1.0);
 }
 // A one-pixel coverage kernel gently feathers silhouettes even when the
 // temporal history is invalid. Contrast gating leaves smooth sky/flat areas
 // alone; the bounded blend keeps tiny blades visible instead of erasing them.
 float edge=smoothstep(.10,.35,(lmax-lmin)/max(lmax,.015));
 vec3 soft=original*.25+(n+s+e+w)*.125+(nw+ne+sw+se)*.0625;
 return mix(c,soft,softness*edge);
}
void main(){
 if(uFrame.miscParams.x>.5){
   // Diagnostic albedo/normals/AO must not pass through split-toning, ink,
   // paper or FXAA; otherwise the debug view cannot diagnose the renderer.
   vec3 raw=texture(uHdr,vUV).rgb;
   outColor=vec4(pow(clamp(raw,0.0,1.0),vec3(1.0/max(pc.gamma,.1))),1);
   return;
 }
 vec3 hdr=mix(filteredHdr(vUV),texture(uBloom,vUV).rgb,clamp(pc.bloomIntensity,0.0,1.0));
 hdr*=pc.gradeTint.rgb*exp2(pc.gradeExposureBias)*pc.exposure;
 float y=lum(hdr),split=smoothstep(.18,.72,y);
 hdr*=mix(uFrame.styleSplitShadow.rgb,uFrame.styleSplitHighlight.rgb,
          smoothstep(uFrame.stylePost0.y-.25,uFrame.stylePost0.y+.25,split));
 float l=lum(hdr);float sat=pc.saturation+uFrame.stylePost0.x;
 hdr=max(mix(vec3(l),hdr,sat),vec3(0));
 // Contrast around middle gray without subtracting away shadow detail.
 hdr=.18*pow(hdr/.18,vec3(max(pc.contrast,.01)));
 float edge=uFrame.stylePost0.w>.001?edgeMask(vUV):0.0;
 if(pc.tonemapMode==0)hdr=painterly(max(hdr,vec3(0)));
 else if(pc.tonemapMode==1)hdr=aces(max(hdr,vec3(0)));
 else if(pc.tonemapMode==2)hdr=max(hdr,vec3(0))/(1.0+max(hdr,vec3(0)));
 else if(pc.tonemapMode==4)hdr=agx(hdr);
 else hdr=clamp(hdr,0.0,1.0);
 hdr=mix(hdr,uFrame.styleOutlineColor.rgb,edge*uFrame.stylePost0.w);
  float vig=1.0-dot(vUV-.5,vUV-.5)*pc.vignette*1.6;hdr*=clamp(vig,0.0,1.0);
  if(uFrame.blizzardParams.w > 0.001){
    float frostStrength = clamp(uFrame.blizzardParams.w, 0.0, 1.0);
    vec2 dEdge = abs(vUV - 0.5) * 2.0;
    float borderDist = mix(max(dEdge.x, dEdge.y), length(vUV - 0.5) * 1.414, 0.5);
    float dendrite = frostPattern(vUV);
    float frostRamp = smoothstep(1.0 - frostStrength * 0.65, 1.02, borderDist + (dendrite - 0.5) * 0.45 * frostStrength);
    vec3 frostColor = vec3(0.85, 0.94, 1.0);
    hdr = mix(hdr, frostColor, frostRamp * 0.88);
  }
 float grain=fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5453)-.5;
 hdr*=1.0+grain*uFrame.stylePost1.w*(.35+.65*lum(hdr));
 hdr=pow(clamp(hdr,0.0,1.0),vec3(1.0/max(pc.gamma,0.1)));
 // Dither at the 8-bit quantisation step. Smooth wide gradients -- the sky is
 // the worst case, and a clear zenith-to-horizon ramp is most of the frame --
 // land on the same output code for many pixels in a row and show as banded
 // contours. This is NOT the film grain above: that one is a style dial
 // (stylePost1.w), multiplicative, and off in most scenes. This is one LSB of
 // triangular-PDF noise applied after the gamma curve, i.e. in the space the
 // quantisation actually happens in, which converts the banding to noise well
 // below the visible threshold. Offset hash so it does not correlate with the
 // grain when both are on.
 float d1=fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5453);
 float d2=fract(sin(dot(gl_FragCoord.xy+vec2(5.31,11.7),vec2(12.9898,78.233)))*43758.5453);
 hdr+=vec3((d1+d2-1.0)*(1.0/255.0));
 outColor=vec4(clamp(hdr,0.0,1.0),1);
}
