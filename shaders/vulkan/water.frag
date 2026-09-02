#version 450
#extension GL_GOOGLE_include_directive : require

// water.frag -- the water surface, without geometry.
//
// WHY NO MESH
// The world streams terrain over an unbounded XZ plane, so a water mesh would
// have needed its own chunking and LOD scheme just to keep up, and every LOD
// seam is a place for a crack to open in a mirror-flat surface. So this is a
// fullscreen pass that finds the surface along each view ray, keeps the hit
// only where it is nearer than whatever the depth buffer already holds, and
// shades it.
//
// OCEAN AND LAKES
// Two surfaces, found two ways. The ocean is one global plane, solved in
// closed form. Lakes each sit at their own altitude, so they come from a
// water-height field (Engine/Terrain/TerrainWater.h) that the CPU samples and
// uploads, and the ray is MARCHED against it, then bisected. The field only
// covers the streamed region; beyond it the ocean plane takes over, which is
// what lets the sea reach the horizon while the field stays small.
//
// WHAT IT SHADES
//   refraction  -- the opaque scene behind the surface, sampled from a copy of
//                  the HDR image, UV-offset by the wave normal, and absorbed
//                  through the water column by Beer-Lambert between the
//                  shallow and deep colours
//   reflection  -- a screen-space march against the same depth buffer, falling
//                  back to the sky cubemap wherever it leaves the screen
//   Fresnel     -- Schlick with F0 = 0.02 (water's real normal-incidence
//                  reflectance), which is what makes the surface glassy at
//                  grazing angles and near-transparent looking straight down
//   sun glint   -- GGX against the wave normal, using the same sunRadiance the
//                  rest of the engine is lit by, so it reddens at sunset
//   foam        -- where the water column is thin, i.e. shorelines
// and finally the whole thing goes through fog.glsl, so water sits in the same
// atmosphere as the terrain instead of floating in front of it.

layout(location=0) in vec2 vNdc;
layout(location=0) out vec4 outColor;

layout(set=0,binding=0) uniform sampler2D uSceneColor; // opaque HDR copy
layout(set=1,binding=0) uniform sampler2D uDepth;      // prepass depth
#define FRAME_DATA_SET 2
#include "frameData.glsl"
#include "skyModel.glsl"
#include "fog.glsl"
layout(set=3,binding=0) uniform samplerCube uEnvMap;
// Water surface altitude over the streamed region. Outside its footprint the
// global sea level applies -- that split is what lets an ocean reach the
// horizon while the field only has to cover the terrain that exists.
layout(set=4,binding=0) uniform sampler2D uWaterField;

layout(push_constant) uniform Push { mat4 invViewProj; } pc;

// Water surface altitude at a world XZ.
//
// The first implementation of this shader intersected ONE analytic plane,
// which is all an ocean needs and all a lake cannot use: lakes sit at their
// own altitudes. TerrainWater on the CPU decides where they are and how high
// they stand, and uploads the answer here; this is just the lookup, so the
// engine keeps a single authority for water exactly as it does for ground.
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

// World position from a screen UV + its depth sample.
vec3 worldFromDepth(vec2 uv,float depth){
  vec4 clip=vec4(uv*2.0-1.0,depth,1.0);
  vec4 w=pc.invViewProj*clip;
  return w.xyz/w.w;
}

// Wave normal. The surface stays geometrically flat -- these octaves perturb
// the NORMAL only. At the ranges this engine renders, a displaced surface
// would buy silhouette detail that the fullscreen formulation cannot express
// anyway (the plane is intersected analytically), whereas the normal is what
// drives every term that actually reads: Fresnel, glint, reflection direction
// and refraction offset.
//
// Octaves are counter-rotated so the field does not visibly travel as one
// sheet in the wind direction, which a straight sum of parallel waves does.
vec3 waveNormal(vec2 p,float t,float fade){
  vec2 dir=uFrame.waterParams2.xy;
  vec2 perp=vec2(-dir.y,dir.x);
  float scale=max(uFrame.waterParams1.y,1e-4);
  float amp=uFrame.waterParams1.x*fade;
  float speed=uFrame.waterParams1.z;
  vec2 grad=vec2(0.0);
  float a=1.0,f=1.0;
  // Central differences on the value noise, in NOISE space. The epsilon has
  // to be a small fraction of a noise cell: an earlier version used
  // 0.55/(scale*f) world units, which works out to 0.55 of a cell -- a
  // difference that wide is a low-pass filter, and it flattened the surface
  // to a mirror no matter what amplitude was dialled in. Dividing by 2e turns
  // the difference into an actual slope so amplitude means the same thing at
  // every octave.
  const float e=0.06;
  for(int i=0;i<4;++i){
    vec2 d=normalize(mix(dir,perp,float(i)*0.27)+vec2(0.0,float(i)*0.11));
    vec2 q=p*scale*f+d*(t*speed*f);
    float nx=wNoise(q+vec2(e,0.0))-wNoise(q-vec2(e,0.0));
    float ny=wNoise(q+vec2(0.0,e))-wNoise(q-vec2(0.0,e));
    grad+=vec2(nx,ny)*(a/(2.0*e));
    a*=0.55;f*=2.1;
  }
  return normalize(vec3(-grad.x*amp,1.0,-grad.y*amp));
}

// Screen-space reflection. Marches the reflected ray in WORLD space and
// projects each step to screen, which keeps the step length uniform in the
// world (a screen-space DDA foreshortens badly on a near-horizontal surface,
// which is every water pixel past a few metres). Returns rgb + a confidence
// in .a; the caller blends the sky in with 1-confidence, so a miss degrades
// to the cubemap instead of to a hole.
vec4 traceSSR(vec3 origin,vec3 dir,float sceneDist){
  int steps=int(uFrame.waterParams1.w);
  // Longer strides far from the camera: screen-space detail there is smaller
  // than a texel anyway, and it buys reach for the same step count.
  float stride=max(0.35,sceneDist*0.035);
  float thickness=uFrame.waterParams2.z;
  vec3 p=origin;
  for(int i=0;i<steps;++i){
    p+=dir*stride;
    vec4 clip=uFrame.viewProj*vec4(p,1.0);
    if(clip.w<=0.0) break;
    vec3 ndc=clip.xyz/clip.w;
    if(any(greaterThan(abs(ndc.xy),vec2(1.0)))) break;
    vec2 uv=ndc.xy*0.5+0.5;
    float sceneDepth=texture(uDepth,uv).r;
    if(sceneDepth>=1.0) continue;         // sky here: keep marching
    vec3 hitPos=worldFromDepth(uv,sceneDepth);
    float alongRay=distance(hitPos,origin);
    float rayLen=distance(p,origin);
    // The sample in front of us is nearer than the ray: we have crossed a
    // surface. Thickness stops the ray from "hitting" the thin sliver behind
    // a foreground object it should have passed by.
    if(rayLen>alongRay&&rayLen-alongRay<thickness+stride){
      // Fade at the screen edges, or reflections pop as geometry that fed
      // them scrolls out of frame.
      vec2 edge=smoothstep(vec2(0.0),vec2(0.12),1.0-abs(ndc.xy));
      float conf=min(edge.x,edge.y);
      return vec4(texture(uSceneColor,uv).rgb,conf);
    }
    stride*=1.06; // geometric growth: near hits stay accurate, reach grows
  }
  return vec4(0.0);
}

void main(){
  vec2 uv=vNdc*0.5+0.5;
  vec3 camPos=uFrame.camPosWS.xyz;

  vec4 farW=pc.invViewProj*vec4(vNdc,1.0,1.0);
  vec3 dir=normalize(farW.xyz/farW.w-camPos);

  // Depth buffer: how far the opaque scene is along this ray.
  float sceneDepth=texture(uDepth,uv).r;
  float sceneDist=1e9;
  vec3 scenePos=vec3(0.0);
  if(sceneDepth<1.0){
    scenePos=worldFromDepth(uv,sceneDepth);
    sceneDist=distance(scenePos,camPos);
  }

  // ---- find the surface ------------------------------------------------
  // With lakes, the water surface is a height FIELD, not one plane, so the
  // ray has to be marched against it. Two shortcuts keep that affordable:
  //
  //  - the ocean is still a plane, so its intersection is solved in closed
  //    form and used directly whenever the ray never enters the field;
  //  - the march only runs while the ray is descending toward water it has
  //    not reached yet, and stops at the opaque scene.
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
    tHit=tPlane;                                  // ocean only
  }else{
    // A ray climbing away from the highest possible water can never meet it.
    float startY=camPos.y;
    float t=0.0;
    float stride=1.5;
    float prevT=0.0;
    bool prevBelow=startY<waterHeightAt(camPos.xz);
    if(prevBelow){
      tHit=0.0;                                   // camera already submerged
    }else{
      const int kMaxSteps=64;
      float marchEnd=min(sceneDist,2000.0);
      for(int i=0;i<kMaxSteps&&t<marchEnd;++i){
        prevT=t;
        t=min(t+stride,marchEnd);
        vec3 p=camPos+dir*t;
        bool below=p.y<waterHeightAt(p.xz);
        if(below){
          // Bisect the bracketing interval: the field is piecewise flat, so a
          // handful of steps lands within centimetres of the true waterline.
          float lo=prevT,hi=t;
          for(int k=0;k<8;++k){
            float mid=(lo+hi)*0.5;
            vec3 pm=camPos+dir*mid;
            if(pm.y<waterHeightAt(pm.xz)) hi=mid; else lo=mid;
          }
          tHit=hi;
          break;
        }
        stride*=1.09;                             // reach without step count
      }
    }
    // Outside the field the ocean plane still applies; take whichever the ray
    // meets first.
    if(tPlane>0.0&&(tHit<0.0||tPlane<tHit)){
      vec3 pp=camPos+dir*tPlane;
      vec2 fuv=(pp.xz-uFrame.waterParams3.yz)/uFrame.waterParams3.w;
      if(any(lessThan(fuv,vec2(0.0)))||any(greaterThan(fuv,vec2(1.0))))
        tHit=tPlane;
    }
  }
  if(tHit<0.0){ outColor=vec4(0.0); return; }

  // Water is only visible where the surface is in front of the opaque scene.
  if(tHit>=sceneDist){ outColor=vec4(0.0); return; }

  vec3 hit=camPos+dir*tHit;
  float level=waterHeightAt(hit.xz);
  vec3 V=-dir;
  bool underwater=camPos.y<level;

  // Wave detail has to fade with distance or it aliases into a shimmering
  // moire well before the horizon -- the surface is near-edge-on out there,
  // so a pixel covers tens of metres of wavefront. Fading the amplitude (not
  // the frequency) leaves a flat, correctly-reflecting surface at range.
  float fade=1.0-smoothstep(120.0,900.0,tHit);
  vec3 N=waveNormal(hit.xz,uFrame.miscParams.z,fade);
  if(underwater) N=-N;

  // Depth of water between the surface and whatever is behind it, along the
  // view ray. This drives absorption AND foam, and it is the quantity that
  // makes shallows read as shallow.
  float column=max(sceneDist-tHit,0.0);
  if(sceneDepth>=1.0) column=1e4; // nothing behind: open water

  // ---- refraction -------------------------------------------------------
  // Offset the lookup by the wave normal's horizontal tilt. Scaled by the
  // column so a shoreline millimetre of water does not smear the sand under
  // it sideways, which is the classic giveaway of a naive refraction offset.
  float refrScale=clamp(column*0.06,0.0,1.0)*0.035;
  vec2 refrUv=clamp(uv+N.xz*refrScale,vec2(0.001),vec2(0.999));
  // Reject the offset if it pulled in a sample that is IN FRONT of the water
  // (an object between camera and surface would otherwise bleed into it).
  float refrDepth=texture(uDepth,refrUv).r;
  if(refrDepth<1.0&&distance(worldFromDepth(refrUv,refrDepth),camPos)<tPlane)
    refrUv=uv;
  vec3 behind=texture(uSceneColor,refrUv).rgb;

  // Beer-Lambert through the column, between the two authored colours.
  // sigma is chosen so the transmitted colour reaches waterDeepColor at
  // `clarity` metres, which is what makes that dial mean something.
  float clarity=uFrame.waterParams0.y;
  vec3 shallow=uFrame.waterShallowColor.rgb;
  vec3 deep=uFrame.waterDeepColor.rgb;
  float absorb=1.0-exp(-column/clarity);
  vec3 medium=mix(shallow,deep,absorb);
  vec3 refracted=mix(behind*mix(vec3(1.0),shallow,0.35),medium,absorb);

  // ---- reflection -------------------------------------------------------
  vec3 R=reflect(dir,N);
  vec3 skyRefl=textureLod(uEnvMap,R,uFrame.waterParams0.z*uFrame.iblParams.y).rgb;
  vec3 reflection=skyRefl;
  if(!underwater&&uFrame.waterParams0.w>0.001){
    // Nudge the origin off the surface so step 0 does not immediately
    // self-intersect the plane it started on.
    vec4 ssr=traceSSR(hit+N*0.05,R,sceneDist);
    reflection=mix(skyRefl,ssr.rgb,ssr.a*clamp(uFrame.waterParams0.w,0.0,1.0));
  }

  // ---- Fresnel ----------------------------------------------------------
  // F0 0.02 is water's real normal-incidence reflectance. Looking straight
  // down you see almost entirely through it; at a graze it becomes a mirror.
  float nv=clamp(dot(N,V),0.0,1.0);
  float fresnel=0.02+0.98*pow(1.0-nv,5.0);
  if(underwater){
    // Seen from below the surface behaves as total internal reflection past
    // the critical angle (~48.6 degrees from vertical for water/air).
    fresnel=mix(fresnel,1.0,smoothstep(0.66,0.75,1.0-nv));
  }

  vec3 color=mix(refracted,reflection,fresnel);

  // ---- sun glint --------------------------------------------------------
  vec3 L=normalize(-uFrame.lightDir.xyz);
  vec3 H=normalize(L+V);
  float rough=uFrame.waterParams0.z;
  float a2=rough*rough*rough*rough;
  float nh=max(dot(N,H),0.0);
  float dTerm=a2/max(3.14159265*pow(nh*nh*(a2-1.0)+1.0,2.0),1e-6);
  float nl=max(dot(N,L),0.0);
  color+=uFrame.sunRadiance.rgb*dTerm*nl*fresnel*0.35;

  // ---- foam -------------------------------------------------------------
  // Thin water = shoreline. Broken up by the same wave clock so the line
  // moves with the surface instead of sitting as a static contour.
  float foamDepth=uFrame.waterParams2.w;
  float foamStrength=uFrame.waterParams3.x;
  if(foamStrength>0.001&&column<foamDepth*4.0){
    // Cubed, so foam is a narrow band hugging the waterline rather than a
    // wash over every shallow area. A linear ramp put ~0.5 foam across a
    // whole flooded flat, which reads as fog on the water, not as surf.
    float edge=1.0-smoothstep(0.0,foamDepth,column);
    edge=edge*edge*edge;
    float churn=wNoise(hit.xz*1.7+uFrame.miscParams.z*0.35)*0.6+
                wNoise(hit.xz*5.3-uFrame.miscParams.z*0.7)*0.4;
    float foam=clamp(edge*smoothstep(0.42,0.85,churn+edge*0.3),0.0,1.0);
    // Foam is bright but not emissive: it is lit by the same sky and sun.
    vec3 foamLit=(textureLod(uEnvMap,vec3(0,1,0),uFrame.iblParams.x).rgb*0.6+
                  uFrame.sunRadiance.rgb*nl*0.35);
    color=mix(color,foamLit,foam*foamStrength);
  }

  // ---- fog --------------------------------------------------------------
  // Same medium as everything else, so a distant water horizon dissolves into
  // the same sky the terrain does instead of staying crisp in front of it.
  vec3 viewDirWS=normalize(hit-camPos);
  vec3 skyAhead=textureLod(uEnvMap,viewDirWS,1.0).rgb;
  vec3 skyAbove=textureLod(uEnvMap,
      vec3(viewDirWS.x,max(viewDirWS.y,0.25),viewDirWS.z),2.0).rgb;
  FogSample fog=fogAlongView(hit,1.0,1.0,vec3(1.0),skyAhead,skyAbove);
  color=fogApply(color,fog);

  int dbg=int(uFrame.miscParams.x+0.5);
  if(dbg==2)color=N*0.5+0.5;
  else if(dbg==3)color=vec3(fog.opacity);
  else if(dbg==6)color=(any(isnan(color))||any(isinf(color)))?vec3(1,0,1):vec3(0);

  // Alpha is coverage: 1 wherever the surface is in front of the scene. The
  // pass is alpha-blended, so every pixel the plane misses costs a blend with
  // alpha 0 rather than a read-modify-write of the whole HDR image.
  outColor=vec4(max(color,vec3(0.0)),1.0);
}
