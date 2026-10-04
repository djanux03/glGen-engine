#version 450
// Scene-linear temporal resolve. History alpha magnitude stores view depth;
// negative marks grass, zero is sky. It is metadata, not opacity/coverage.
// Keep this before bloom and editor UI: neither is a surface to reproject.
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outHistory;
layout(set=0,binding=0) uniform sampler2D uCurrent;
layout(set=0,binding=1) uniform sampler2D uDepth;
layout(set=0,binding=2) uniform sampler2D uHistory;
layout(push_constant) uniform Push { mat4 invViewProj; mat4 previousViewProj; } pc;

vec3 ycocg(vec3 c){return vec3(dot(c,vec3(.25,.5,.25)),(c.r-c.b)*.5,(-c.r+2*c.g-c.b)*.25);}
vec3 rgb(vec3 c){return vec3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
void main(){
 float depth=texture(uDepth,vUV).r;
 vec3 current=texture(uCurrent,vUV).rgb;
 vec4 world=pc.invViewProj*vec4(vUV*2.0-1.0,depth,1.0);
 // Storing device Z in FP16 merged whole distant banks into one depth bin.
 // Perspective clip W is linear view depth; zero is the sky sentinel.
 float viewDepth=depth>=1.0?0.0:1.0/world.w;
 // Carry the CURRENT surface tag, never accumulate it with yesterday's colour.
 // Magnitude remains linear depth, so grass retains normal disocclusion checks.
 if(texture(uCurrent,vUV).a<0.0)viewDepth=-viewDepth;
 outHistory=vec4(current,viewDepth);
 world/=world.w;
 if(depth>=1.0){
   // A sky ray has rotation but no translation. Treating the far plane as
   // a cloud surface made the sky swim when walking across the shoreline.
   vec4 eye=pc.invViewProj*vec4(0,0,1,0);
   world=vec4(world.xyz-eye.xyz/eye.w,0.0);
 }
 vec4 previous=pc.previousViewProj*world;
 if(previous.w<=0.0)return; // zero matrix also invalidates a camera cut
 vec3 ndc=previous.xyz/previous.w;
 vec2 uv=ndc.xy*.5+.5;
 if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1))))return;
 vec4 history=texture(uHistory,uv);
 // Colour may be bilinear, but interpolating depth invents a surface between
 // a leaf and its background. That rejected history on nearly every thin
 // grass/branch edge and left those silhouettes jagged despite AA being on.
 // Validate against the actual four depths that contribute to filtered colour.
 ivec2 size=textureSize(uHistory,0),base=ivec2(floor(uv*vec2(size)-.5));
 float depthError=1e20;
 for(int y=0;y<2;++y)for(int x=0;x<2;++x){
   float oldDepth=abs(texelFetch(uHistory,clamp(base+ivec2(x,y),ivec2(0),size-1),0).a);
   if(depth>=1.0){if(oldDepth<=0.0)depthError=0.0;}
   else if(oldDepth>0.0)depthError=min(depthError,abs(oldDepth-previous.w));
 }
 if(depthError>max(.05,previous.w*.005))return;

 // Clip in luminance/chroma space so a moving leaf cannot drag the bright
 // sky behind it. Depth rejection handles disocclusion; the local envelope
 // also bounds wind/animated foliage, which has no velocity stream yet.
 vec2 px=1.0/vec2(textureSize(uCurrent,0));
 vec3 lo=ycocg(current),hi=lo;
 vec3 mean=vec3(0),moment=vec3(0);
 for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
   vec3 c=ycocg(texture(uCurrent,vUV+vec2(x,y)*px).rgb);
   lo=min(lo,c);hi=max(hi,c);
   mean+=c;moment+=c*c;
 }
 mean/=9.0;moment/=9.0;
 vec3 sigma=sqrt(max(moment-mean*mean,vec3(0)));
 // A min/max box alone lets one bright neighbour retain a whole trail.
 // Variance clipping preserves repeated fine texture while rejecting outliers.
 lo=max(lo,mean-1.25*sigma);hi=min(hi,mean+1.25*sigma);
 vec3 centre=(lo+hi)*.5,extent=max((hi-lo)*.5,vec3(1e-5));
 vec3 delta=ycocg(history.rgb)-centre;
 float outside=max(abs(delta.x)/extent.x,max(abs(delta.y)/extent.y,abs(delta.z)/extent.z));
 vec3 past=rgb(centre+delta/max(outside,1.0));
 float motion=length((uv-vUV)/px);
 float weight=mix(.88,.65,smoothstep(0.5,8.0,motion));
 float change=abs(ycocg(past).x-ycocg(current).x)/max(max(ycocg(past).x,ycocg(current).x),.05);
 weight*=1.0-.55*smoothstep(.08,.35,change);
 outHistory=vec4(max(mix(current,past,weight),vec3(0)),viewDepth);
}
