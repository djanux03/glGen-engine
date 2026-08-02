#version 450
#extension GL_GOOGLE_include_directive : require
#define FRAME_DATA_SET 0
#include "frameData.glsl"
#include "skyModel.glsl"
layout(location=0) in vec2 vNdc;
layout(location=0) out vec4 outColor;
layout(push_constant) uniform Push {
 mat4 invViewProj; vec4 sunDir; vec4 moonDir; vec4 camPos; vec4 passFlags;
} pc;
float hash31(vec3 p){p=fract(p*.1031);p+=dot(p,p.yzx+33.33);return fract((p.x+p.y)*p.z);}
float noise3(vec3 p){vec3 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);
 float a=hash31(i),b=hash31(i+vec3(1,0,0)),c=hash31(i+vec3(0,1,0)),d=hash31(i+vec3(1,1,0));
 float e=hash31(i+vec3(0,0,1)),g=hash31(i+vec3(1,0,1)),h=hash31(i+vec3(0,1,1)),j=hash31(i+1.0);
 return mix(mix(mix(a,b,f.x),mix(c,d,f.x),f.y),mix(mix(e,g,f.x),mix(h,j,f.x),f.y),f.z);}
float fbm(vec3 p){return noise3(p)*.56+noise3(p*2.03+7.1)*.28+noise3(p*4.07-3.7)*.16;}
vec3 painterlyGrade(vec3 physical,vec3 dir){
 float horizon=pow(1.0-clamp(dir.y,0.0,1.0),1.7);
 vec3 palette=mix(uFrame.styleSkyZenith.rgb,uFrame.styleSkyHorizon.rgb,horizon);
 float y=dot(max(physical,vec3(0)),vec3(.2126,.7152,.0722));
 vec3 chroma=physical/max(y,.001);float bands=max(uFrame.styleSky0.y,2.0);
 float q=floor(y*bands+.5)/bands;float softness=clamp(uFrame.styleSky0.z,0.01,.49);
 q=mix(q,y,softness);vec3 washed=palette*mix(.42,1.30,q)+chroma*q*.22;
 return mix(physical,washed,uFrame.styleSky0.x);
}
void main(){
 vec4 fw=pc.invViewProj*vec4(vNdc,1,1);vec3 dir=normalize(fw.xyz/fw.w-pc.camPos.xyz);
 vec3 toSun=normalize(pc.sunDir.xyz),toMoon=normalize(pc.moonDir.xyz);
 vec3 ro=atmPlanetPos(pc.camPos.y);
 AtmSample a=atmScatter(ro,dir,toSun,vec3(pc.sunDir.w),toMoon,
   vec3(.72,.82,1)*pc.moonDir.w,uFrame.styleSkyZenith.w,12);
 vec3 color=a.radiance*uFrame.styleSkyHorizon.w;
 float sunUp=smoothstep(-.14,.02,toSun.y);
 color+=vec3(.0022,.0031,.0058)*uFrame.skyAmbientParams.x*(1.0-sunUp)*a.transmittance;
 bool sky=a.groundT<0.0;bool env=pc.passFlags.x>.5;
 if(sky&&dir.y>-.03){
   vec3 cp=dir/max(dir.y+.28,.12)*1.35;
   cp.xz+=uFrame.styleCloud0.zw*pc.camPos.w;
   float warp=fbm(cp*.55+vec3(4,0,7));float n=fbm(cp+vec3(warp*1.7,0,warp));
   float cov=clamp(uFrame.styleCloud0.x,0.0,.95),soft=max(uFrame.styleCloud0.y,.01);
   float mask=smoothstep(1.0-cov-soft,1.0-cov+soft,n)*smoothstep(-.02,.18,dir.y);
   float light=smoothstep(.20,.82,dot(dir,toSun)*.5+.5);
   vec3 cloud=mix(uFrame.styleCloudBase.rgb,uFrame.styleCloudMid.rgb,clamp(n*1.3-.2,0,1));
   cloud=mix(cloud,uFrame.styleCloudLit.rgb,light);
   color=mix(color,cloud*mix(.35,1.35,sunUp),mask*.88);
 }
 color=painterlyGrade(color,dir);
 if(!env&&sky){
   float ang=acos(clamp(dot(dir,toSun),-1,1));float radius=.009;
   float edge=1.0-smoothstep(radius*(.55-uFrame.styleSky0.w*.2),radius,ang);
   color+=vec3(1.0,.86,.60)*pc.sunDir.w*uFrame.styleCloudMid.w*edge*a.transmittance;
   float mr=max(pc.passFlags.y,1e-4),ma=acos(clamp(dot(dir,toMoon),-1,1));
   color+=vec3(.88,.92,1.0)*pc.moonDir.w*18.0*(1.0-smoothstep(mr*.82,mr,ma))*a.transmittance;
   float star=(hash31(floor(dir*180.0))>.992?1.0:0.0)*(1.0-sunUp);
   color+=vec3(.7,.82,1.0)*star*uFrame.styleCloudLit.w*.18;
 }
 outColor=vec4(max(color,vec3(0)),1);
}
