#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// water.frag -- the water and frozen glacial ice surface, without geometry.
// Supports both liquid ocean/lakes and frozen glacial ice (The Long Dark iceMode).

layout(location=0) in vec2 vNdc;
layout(location=0) out vec4 outColor;

layout(set=0,binding=0) uniform sampler2D uSceneColor; // opaque HDR copy
layout(set=1,binding=0) uniform sampler2D uDepth;      // prepass depth
#define FRAME_DATA_SET 2
#include "frameData.glsl"
#include "skyModel.glsl"
#define SKY_LUT_SET 6
#include "skyLutSample.glsl"
#define FOG_AERIAL_ONLY
#include "fog.glsl"
#define ATMOSPHERE_SET 6
#include "atmosphereData.glsl"
layout(set=3,binding=0) uniform samplerCube uEnvMap;
#define CLOUD_SHADOW_SET 3
#include "cloudShadow.glsl"
#define uEnvironment uEnvMap
#include "fogSample.glsl"
layout(set=4,binding=0) uniform sampler2D uWaterField;
layout(set=5,binding=0) uniform accelerationStructureEXT uTLAS;
#include "surfaceShadow.glsl"

layout(push_constant) uniform Push { mat4 invViewProj; } pc;

float waterHeightAt(vec2 xz){
  float sea=uFrame.waterParams0.x;
  float fieldSize=uFrame.waterParams3.w;
  if(fieldSize<=0.0) return sea;              // no field: ocean only
  vec2 uv=(xz-uFrame.waterParams3.yz)/fieldSize;
  if(any(lessThan(uv,vec2(0.0)))||any(greaterThan(uv,vec2(1.0)))) return sea;
  return texture(uWaterField,uv).r;
}

float wHash(vec2 p){
  vec3 p3=fract(vec3(p.xyx)*0.1031);
  p3+=dot(p3,p3.yzx+33.33);
  return fract((p3.x+p3.y)*p3.z);
}
float wNoise(vec2 p){
  vec2 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);
  float a=wHash(i),b=wHash(i+vec2(1,0)),c=wHash(i+vec2(0,1)),d=wHash(i+vec2(1,1));
  return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
}

// Subsurface fracture lines for frozen lake ice
float iceCracks(vec2 p){
  vec2 g=floor(p), f=fract(p);
  float minDist=1.0, secondDist=1.0;
  for(int y=-1; y<=1; ++y){
    for(int x=-1; x<=1; ++x){
      vec2 cell=vec2(float(x),float(y));
      vec2 pt=cell+vec2(wHash(g+cell), wHash(g+cell+vec2(17.3,31.7)));
      float d=length(f-pt);
      if(d<minDist){
        secondDist=minDist;
        minDist=d;
      }else if(d<secondDist){
        secondDist=d;
      }
    }
  }
  float crack=secondDist-minDist;
  return 1.0-smoothstep(0.015,0.08,crack);
}

vec3 worldFromDepth(vec2 uv,float depth){
  vec4 clip=vec4(uv*2.0-1.0,depth,1.0);
  vec4 w=pc.invViewProj*clip;
  return w.xyz/w.w;
}

vec3 waveNormal(vec2 p,float t,float fade,float footprint){
  vec2 dir=uFrame.waterParams2.xy;
  float scale=max(uFrame.waterParams1.y,1e-4);
  float amp=uFrame.waterParams1.x*fade;
  float speed=uFrame.waterParams1.z;
  vec2 grad=vec2(0.0);
  float a=1.0,f=1.0;
  const float angles[6]=float[6](0.0,.83,-.64,1.47,-1.13,.37);
  for(int i=0;i<6;++i){
    float angle=angles[i];
    vec2 d=mat2(cos(angle),sin(angle),-sin(angle),cos(angle))*normalize(dir+vec2(1e-5));
    float k=6.2831853*scale*f;
    // Deep-water dispersion: small waves travel more slowly than swell.
    float phase=dot(p,d)*k+t*speed*sqrt(9.81*k)+float(i)*2.399963;
    // Slow phase warp breaks long coherent sine ridges into wind ripples.
    phase+=wNoise(p*scale*.73+float(i)*vec2(3.7,5.1))*3.0;
    float resolved=1.0-smoothstep(.25,.75,footprint*scale*f);
    grad+=d*(cos(phase)*a*k*resolved);
    a*=0.48;f*=1.9;
  }
  return normalize(vec3(-grad.x*amp,1.0,-grad.y*amp));
}

vec4 traceSSR(vec3 origin,vec3 dir,float sceneDist){
  int steps=int(uFrame.waterParams1.w);
  float stride=max(0.35,sceneDist*0.035);
  float thickness=uFrame.waterParams2.z;
  vec3 p=origin;
  vec3 previous=p;
  for(int i=0;i<steps;++i){
    previous=p;
    p+=dir*stride;
    vec4 clip=uFrame.viewProj*vec4(p,1.0);
    if(clip.w<=0.0) break;
    vec3 ndc=clip.xyz/clip.w;
    if(any(greaterThan(abs(ndc.xy),vec2(1.0)))) break;
    vec2 uv=ndc.xy*0.5+0.5;
    float sceneDepth=texture(uDepth,uv).r;
    if(sceneDepth>=1.0) continue;
    vec3 hitPos=worldFromDepth(uv,sceneDepth);
    // The depth buffer contains the submerged bed (water is a later pass).
    // It cannot appear in an upward reflection ray; treating it as a hit
    // painted noisy horizontal ground patterns over the reflected sky.
    if(hitPos.y<waterHeightAt(hitPos.xz)-0.05){stride*=1.06;continue;}
    // Compare camera depth at the projected pixel, not distance from the
    // reflection origin. Those radial distances selected unrelated banks,
    // producing glittering streaks instead of reflected tree silhouettes.
    float delta=length(p-uFrame.camPosWS.xyz)-length(hitPos-uFrame.camPosWS.xyz);
    if(delta>0.0&&delta<thickness+stride){
      // Refine the crossing so a large marching step does not smear a thin
      // crown over the water. Misses fade into the environment smoothly.
      vec3 lo=previous,hi=p;
      for(int k=0;k<5;++k){
        vec3 mid=(lo+hi)*.5;
        vec4 mc=uFrame.viewProj*vec4(mid,1.0);
        vec2 mu=mc.xy/mc.w*.5+.5;
        float md=texture(uDepth,mu).r;
        vec3 mh=worldFromDepth(mu,md);
        if(md<1.0&&length(mid-uFrame.camPosWS.xyz)>length(mh-uFrame.camPosWS.xyz))hi=mid;
        else lo=mid;
      }
      vec4 hc=uFrame.viewProj*vec4(hi,1.0);
      uv=hc.xy/hc.w*.5+.5;
      vec2 edge=smoothstep(vec2(0.0),vec2(0.12),1.0-abs(ndc.xy));
      float conf=min(edge.x,edge.y)*(1.0-smoothstep(thickness,thickness+stride,delta));
      vec3 reflectedHit=worldFromDepth(uv,texture(uDepth,uv).r);
      vec3 reflection=applyReflectionAtmosphere(texture(uSceneColor,uv).rgb,
          origin,dir,length(reflectedHit-origin));
      return vec4(reflection,conf);
    }
    stride*=1.06;
  }
  return vec4(0.0);
}

void main(){
  vec2 uv=vNdc*0.5+0.5;
  vec3 camPos=uFrame.camPosWS.xyz;

  vec4 farW=pc.invViewProj*vec4(vNdc,1.0,1.0);
  vec3 dir=normalize(farW.xyz/farW.w-camPos);
  // Derivatives before any water/shoreline early-outs.
  float rayFootprint=max(length(dFdx(dir)),length(dFdy(dir)));

  float sceneDepth=texture(uDepth,uv).r;
  float sceneDist=1e9;
  vec3 scenePos=vec3(0.0);
  if(sceneDepth<1.0){
    scenePos=worldFromDepth(uv,sceneDepth);
    sceneDist=distance(scenePos,camPos);
  }

  float sea=uFrame.waterParams0.x;
  float tPlane=-1.0;
  {
    float denom=dir.y;
    if(abs(denom)>1e-5){
      float t=(sea-camPos.y)/denom;
      if(t>0.0) tPlane=t;
    }
  }

  float tHit=-1.0;
  if(uFrame.waterParams3.w<=0.0){
    tHit=tPlane;
  }else{
    float startY=camPos.y;
    float t=0.0;
    float stride=1.5;
    float prevT=0.0;
    bool prevBelow=startY<waterHeightAt(camPos.xz);
    if(prevBelow){
      tHit=0.0;
    }else{
      const int kMaxSteps=64;
      float marchEnd=min(sceneDist,2000.0);
      for(int i=0;i<kMaxSteps&&t<marchEnd;++i){
        prevT=t;
        t=min(t+stride,marchEnd);
        vec3 p=camPos+dir*t;
        bool below=p.y<waterHeightAt(p.xz);
        if(below){
          float lo=prevT,hi=t;
          for(int k=0;k<8;++k){
            float mid=(lo+hi)*0.5;
            vec3 pm=camPos+dir*mid;
            if(pm.y<waterHeightAt(pm.xz)) hi=mid; else lo=mid;
          }
          tHit=hi;
          break;
        }
        stride*=1.09;
      }
    }
    if(tPlane>0.0&&(tHit<0.0||tPlane<tHit)){
      vec3 pp=camPos+dir*tPlane;
      vec2 fuv=(pp.xz-uFrame.waterParams3.yz)/uFrame.waterParams3.w;
      if(any(lessThan(fuv,vec2(0.0)))||any(greaterThan(fuv,vec2(1.0))))
        tHit=tPlane;
    }
  }
  if(tHit<0.0){ outColor=vec4(0.0); return; }
  if(tHit>=sceneDist){ outColor=vec4(0.0); return; }

  vec3 hit=camPos+dir*tHit;
  float level=waterHeightAt(hit.xz);
  vec3 V=-dir;
  bool underwater=camPos.y<level;

  bool isIce=uFrame.iceParams.x>0.5;
  float fade=1.0-smoothstep(120.0,900.0,tHit);
  vec3 N;
  if(isIce){
    // Frozen rigid ice: subtle static plate undulations without traveling wave motion
    float n1=wNoise(hit.xz*0.18)-0.5;
    float n2=wNoise(hit.xz*0.18+vec2(11.3,7.9))-0.5;
    N=normalize(vec3(n1*0.04, 1.0, n2*0.04));
  }else{
    N=waveNormal(hit.xz,uFrame.miscParams.z,fade,rayFootprint*tHit/max(abs(dir.y),.1));
  }
  if(underwater) N=-N;

  float column=max(sceneDist-tHit,0.0);
  if(sceneDepth>=1.0) column=1e4;

  // ---- refraction ----
  float refrScale=clamp(column*0.06,0.0,1.0)*(isIce ? 0.008 : 0.035);
  vec2 refrUv=clamp(uv+N.xz*refrScale,vec2(0.001),vec2(0.999));
  float refrDepth=texture(uDepth,refrUv).r;
  // Test against this water hit, including lake fields (tPlane may be -1).
  if(refrDepth<1.0&&distance(worldFromDepth(refrUv,refrDepth),camPos)<tHit)
    refrUv=uv;
  vec3 behind=texture(uSceneColor,refrUv).rgb;

  // Beer-Lambert absorption
  float clarity=isIce ? max(uFrame.iceParams.w, 0.5) : max(uFrame.waterParams0.y, 0.01);
  vec3 shallow=isIce ? uFrame.iceColor.rgb : uFrame.waterShallowColor.rgb;
  vec3 deep=isIce ? (uFrame.iceColor.rgb * vec3(0.32, 0.65, 0.85)) : uFrame.waterDeepColor.rgb;
  float absorb=1.0-exp(-column/clarity);
  vec3 medium=mix(shallow,deep,absorb);
  vec3 refracted=mix(behind*mix(vec3(1.0),shallow,0.35),medium,absorb);
  vec3 L=normalize(-uFrame.lightDir.xyz);
  float nl=max(dot(N,L),0.0);
  float lightVisibility=1.0;
  if(!underwater&&nl>0.0&&uFrame.lightParams.y>.001)
    lightVisibility=mix(1.0,traceShadow(hit+vec3(0,.025,0),L),uFrame.lightParams.y);
  lightVisibility*=cloudTransmission(hit);
  if(!isIce){
    // Spectral Beer-Lambert extinction. Scalar lerp made a 20 cm shallows
    // and a deep pond share a flat tint. Red dies first; authored green/brown
    // water colours still control scattering from suspended organic matter.
    vec3 extinction=vec3(1.35,.65,.85)/clarity;
    vec3 transmission=exp(-extinction*min(column,10000.0));
    vec3 illumination=textureLod(uEnvMap,vec3(0,1,0),uFrame.iblParams.x).rgb*
                      max(uFrame.lightParams.x,0.0)+
                      uFrame.sunRadiance.rgb*(nl*lightVisibility/3.14159265);
    refracted=behind*transmission+medium*illumination*(1.0-transmission);
  }

  float rough=clamp(isIce ? uFrame.iceColor.w : uFrame.waterParams0.z, 0.04, 1.0);
  if(isIce){
    // Subsurface fracture lines (ice cracks)
    float cracks=(iceCracks(hit.xz*0.08)*0.75+iceCracks(hit.xz*0.42)*0.35)*uFrame.iceParams.y;
    refracted+=vec3(0.75,0.92,1.0)*cracks*0.65*(1.0-absorb*0.4);

    // Surface frost & shoreline riming
    float frostNoise=wNoise(hit.xz*0.4)*0.65+wNoise(hit.xz*1.8)*0.35;
    float frostShore=1.0-smoothstep(0.0,1.5,column);
    float frost=clamp(smoothstep(1.0-uFrame.iceParams.z,1.15-uFrame.iceParams.z,frostNoise)+frostShore*0.65,0.0,1.0);
    vec3 frostColor=vec3(0.88,0.94,0.98);
    refracted=mix(refracted,frostColor,frost*0.75);
    rough=mix(rough,0.82,frost*0.8);
  }

  // ---- reflection ----
  vec3 R=reflect(dir,N);
  vec3 skyRefl=applyReflectionAtmosphere(textureLod(uEnvMap,R,rough*uFrame.iblParams.y).rgb,hit,R,4000.0);
  vec3 reflection=skyRefl;
  if(!underwater&&uFrame.waterParams0.w>0.001){
    vec4 ssr=traceSSR(hit+N*0.05,R,sceneDist);
    reflection=mix(skyRefl,ssr.rgb,ssr.a*clamp(uFrame.waterParams0.w,0.0,1.0)*(1.0-rough));
  }

  // ---- Fresnel ----
  float f0=isIce ? 0.04 : 0.02;
  float nv=clamp(dot(N,V),0.0,1.0);
  float fresnel=f0+(1.0-f0)*pow(1.0-nv,5.0);
  if(underwater){
    fresnel=mix(fresnel,1.0,smoothstep(0.66,0.75,1.0-nv));
  }

  vec3 color=mix(refracted,reflection,fresnel);

  // ---- sun glint ----
  vec3 H=(L+V)/max(length(L+V),1e-5);
  float a2=rough*rough*rough*rough;
  float nh=max(dot(N,H),0.0);
  float dTerm=a2/max(3.14159265*pow(nh*nh*(a2-1.0)+1.0,2.0),1e-6);
  // Cook-Torrance masking keeps grazing microfacets bounded. Using D alone
  // overexposed tiny normals into white speckles across otherwise still water.
  float gv=nl*sqrt(nv*nv*(1.0-a2)+a2),gl=nv*sqrt(nl*nl*(1.0-a2)+a2);
  float visibility=.5/max(gv+gl,1e-6);
  float fh=f0+(1.0-f0)*pow(1.0-max(dot(H,V),0.0),5.0);
  color+=uFrame.sunRadiance.rgb*dTerm*visibility*fh*nl*lightVisibility;

  // ---- shoreline / foam ----
  float foamDepth=uFrame.waterParams2.w;
  float foamStrength=uFrame.waterParams3.x;
  if(isIce){
    float shoreRim=1.0-smoothstep(0.0,max(foamDepth*2.5,0.1),column);
    vec3 rimColor=vec3(0.90,0.95,0.99);
    color=mix(color,rimColor,shoreRim*0.65);
  }else if(foamStrength>0.001&&column<foamDepth*4.0){
    float edge=1.0-smoothstep(0.0,foamDepth,column);
    edge=edge*edge*edge;
    float churn=wNoise(hit.xz*1.7+uFrame.miscParams.z*0.35)*0.6+
                wNoise(hit.xz*5.3-uFrame.miscParams.z*0.7)*0.4;
    float foam=clamp(edge*smoothstep(0.42,0.85,churn+edge*0.3),0.0,1.0);
    vec3 foamLit=(textureLod(uEnvMap,vec3(0,1,0),uFrame.iblParams.x).rgb*0.6+
                  uFrame.sunRadiance.rgb*nl*lightVisibility*0.35);
    color=mix(color,foamLit,foam*foamStrength);
  }

  // ---- fog ----
  vec3 viewDirWS=normalize(hit-camPos);
  vec3 skyAhead=textureLod(uEnvMap,viewDirWS,1.0).rgb;
  vec3 skyAbove=textureLod(uEnvMap,
      vec3(viewDirWS.x,max(viewDirWS.y,0.25),viewDirWS.z),2.0).rgb;
  FogSample fog=fogAlongView(hit,1.0,1.0,vec3(1.0),skyAhead,skyAbove);
  if(uAtmosphere.dustAlbedo.w>=0) color=vec3(uAtmosphere.dustAlbedo.w);
  color=applyCameraAtmosphere(color,hit,false);

  int dbg=int(uFrame.miscParams.x+0.5);
  if(dbg==2)color=N*0.5+0.5;
  else if(dbg==3)color=vec3(fog.opacity);
  else if(dbg==6)color=(any(isnan(color))||any(isinf(color)))?vec3(1,0,1):vec3(0);

  outColor=vec4(max(color,vec3(0.0)),1.0);
}
