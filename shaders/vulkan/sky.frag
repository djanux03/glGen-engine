#version 450
#extension GL_GOOGLE_include_directive : require
#define FRAME_DATA_SET 0
#include "frameData.glsl"
#include "skyModel.glsl"
#include "fog.glsl"
layout(location=0) in vec2 vNdc;
layout(location=0) out vec4 outColor;
// Half-res output of the volumetric cloud march (clouds.frag): rgb =
// premultiplied scattered light, a = transmittance. Always bound, even when
// the cloud assets failed to load or the layer is switched off -- the buffer
// is cleared to (0,0,0,1) in that case, which makes the blend below an exact
// no-op. passFlags.z carries the strength dial (0 = skip).
layout(set=1, binding=0) uniform sampler2D uCloud;
layout(push_constant) uniform Push {
 mat4 invViewProj; vec4 sunDir; vec4 moonDir; vec4 camPos; vec4 passFlags;
} pc;
float hash31(vec3 p){p=fract(p*.1031);p+=dot(p,p.yzx+33.33);return fract((p.x+p.y)*p.z);}
float noise3(vec3 p){vec3 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);
 float a=hash31(i),b=hash31(i+vec3(1,0,0)),c=hash31(i+vec3(0,1,0)),d=hash31(i+vec3(1,1,0));
 float e=hash31(i+vec3(0,0,1)),g=hash31(i+vec3(1,0,1)),h=hash31(i+vec3(0,1,1)),j=hash31(i+1.0);
 return mix(mix(mix(a,b,f.x),mix(c,d,f.x),f.y),mix(mix(e,g,f.x),mix(h,j,f.x),f.y),f.z);}
float fbm(vec3 p){return noise3(p)*.47+noise3(p*2.03+7.1)*.25+
 noise3(p*4.07-3.7)*.16+noise3(p*8.11+13.2)*.08+noise3(p*16.23-8.4)*.04;}
// Two octaves, for the toward-the-sun shadow tap only. That tap exists to
// darken cloud cores relative to their sun-facing edges; it does not need the
// high frequencies, and running the full five here doubled the sky pass cost
// for a difference that was not visible.
float fbm2(vec3 p){return noise3(p)*.65+noise3(p*2.11+5.3)*.35;}

// Authored illustrative wash over the physical sky: quantises luminance into
// bands and pulls the result toward a two-colour zenith/horizon palette.
// Unchanged -- this is the engine's house look, not a defect. It runs BEFORE
// the sun/moon/stars so those stay physical highlights on top of the wash.
vec3 painterlyGradeSky(vec3 physical,vec3 dir){
 float horizon=pow(1.0-clamp(dir.y,0.0,1.0),1.7);
 vec3 palette=mix(uFrame.styleSkyZenith.rgb,uFrame.styleSkyHorizon.rgb,horizon);
 float y=dot(max(physical,vec3(0)),vec3(.2126,.7152,.0722));
 vec3 chroma=physical/max(y,.001);
 // Smooth continuous luminance (eliminates stair-step quantization bands across sky and clouds)
 float q=y;
 float lift=smoothstep(0.0,0.015,y);
 vec3 washed=palette*mix(.42,1.30,q)*lift+chroma*q*.22;
 return mix(physical,washed,uFrame.styleSky0.x);
}

// ---------------------------------------------------------------------------
// Cloud deck
// ---------------------------------------------------------------------------
// The deck is a SPHERE concentric with the planet, intersected with the view
// ray, rather than the flat plane this shader used to project onto
// (dir/max(dir.y+.28,.12)). A flat plane cannot reach the horizon: its
// projection was floored to keep it finite, so cloud features stopped growing
// near the horizon and the shader faded them out with
// smoothstep(-.02,.18,dir.y) to hide the fact. Real cloud decks do the
// opposite -- they compress into a dense band at the horizon and end at a
// hard geometric edge. A sphere gives both for free, and gives the ray a
// well-defined miss (below the horizon) instead of a fade.
struct CloudHit { bool hit; vec2 uv; float dist; };
CloudHit cloudDeck(vec3 ro, vec3 dir) {
    CloudHit c; c.hit = false; c.uv = vec2(0.0); c.dist = 0.0;
    float deck = kAtmRg + max(uFrame.styleCloud1.x, 50.0);
    vec2 t = atmRaySphere(ro, dir, deck);
    // Far root: the deck overhead. A camera above the deck would want the
    // near root instead, but this engine's cameras are terrain-bound and the
    // near root is behind them (negative) at every altitude they reach.
    if (t.y <= 0.0)
        return c;
    vec3 p = ro + dir * t.y;
    c.hit = true;
    c.dist = t.y;
    c.uv = p.xz / max(uFrame.styleCloud1.y, 1.0);
    return c;
}

// Coverage remap. The raw fbm is a sum of five hash-noise octaves, so it is
// bunched hard around 0.5 -- feeding it straight into
// smoothstep(1-cov-soft, 1-cov+soft, n) meant the DEFAULT coverage of 0.47
// rendered as near-total overcast (measured ~0.72 mean mask, and it looked
// like more). Stretching the field about its mean first makes the dial mean
// roughly what it says: 0 clear, 1 solid.
float cloudCoverageMask(float n, float cov, float soft) {
    n = clamp((n - 0.5) * 2.35 + 0.5, 0.0, 1.0);
    float thr = 1.0 - clamp(cov, 0.0, 1.0);
    return smoothstep(thr - soft, thr + soft, n);
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
 float mu=dot(dir,toSun);
 float cloudMask=0.0;
 // The analytic deck below now runs ONLY for the environment-cubemap faces.
 // The visible sky's clouds are the volumetric layer (clouds.frag), which is
 // marched at half res and composited over this pass -- drawing this one too
 // would put two decks in the frame. The cubemap keeps it because IBL is
 // rendered at 128x128x6 every frame and cannot afford a second volumetric
 // march; a cheap deck there is still far better than a clear sky, since it
 // is what tints the ambient light reaching every surface.
 CloudHit deck=cloudDeck(ro,dir);
 if(env&&sky&&deck.hit){
   vec3 cp=vec3(deck.uv.x,0.0,deck.uv.y);
   cp.xz+=uFrame.styleCloud0.zw*pc.camPos.w;
   float warp=fbm(cp*.31+vec3(4,0,7));
   float body=fbm(cp*.78+vec3(warp*2.2,0,warp*1.7));
   float erosion=fbm(cp*2.65+vec3(-warp,3.0,warp))*0.28;
   float n=clamp(body*.86+erosion,0.0,1.0);
   float cov=clamp(uFrame.styleCloud0.x,0.0,1.0),soft=max(uFrame.styleCloud0.y,.01);
   float mask=cloudCoverageMask(n,cov,soft);
   cloudMask=mask;
   // A ray leaving at elevation theta crosses a deck of thickness d over
   // d/sin(theta) of cloud, so grazing views see far more of it -- that is
   // why a broken sky overhead still closes into a solid band at the horizon.
   // This belongs on optical depth, not on coverage: pushing the COVERAGE up
   // instead (the first thing tried here) fabricated cloud where the field
   // said there was none, and produced a hard white ring.
   float graze=clamp(1.0/max(dir.y,0.06),1.0,6.0);

   if(mask>0.001){
     // ---- lit clouds --------------------------------------------------
     // The old model was mix(base,mid,n) then mix(->lit, rim) then a flat
     // day/night scale. Nothing in it depended on where the sun actually
     // was, so clouds stayed the same dirty beige at noon and at sunset and
     // never produced a silver lining. This is Beer-Lambert through the
     // deck with a forward-scatter lobe, driven by sunRadiance -- which is
     // already transmittance-coloured on the CPU, so the clouds go white at
     // noon and gold at dusk with no special case.
     float tau=n*max(uFrame.styleCloud1.z,0.01)*6.0*graze;
     // One tap offset toward the sun estimates how much cloud sits between
     // this point and the light: sun-facing flanks stay bright, cores go
     // dark. Offset is in deck-plane units, so it shortens as the sun rises.
     vec2 sunStep=toSun.xz*(1.0/max(abs(toSun.y),0.22))*0.55;
     float nSun=clamp(fbm2(vec3(cp.x+sunStep.x,0.0,cp.z+sunStep.y)*.78)*1.15,0.0,1.0);
     float tauSun=nSun*max(uFrame.styleCloud1.z,0.01)*6.0*uFrame.styleCloud1.w;
     float lit=exp(-tauSun);
     // Powder term: thin edges scatter less back at you than their optical
     // depth alone suggests, which is what carves the bright fringe.
     float powder=1.0-exp(-tau*2.0);
     float phase=atmPhaseHG(mu,0.62)*12.566371; // back to a unitless lobe
     vec3 direct=uFrame.sunRadiance.rgb*lit*mix(0.55,1.0,powder)*
                 mix(0.7,1.9,clamp(phase,0.0,2.0));
     // Ambient: the sky the cloud sits in, darkening into the base.
     vec3 ambientSky=a.radiance*uFrame.styleSkyHorizon.w;
     vec3 ambient=ambientSky*mix(0.35,1.05,1.0-n);
     // Style tints stay, as tints on a physical result rather than as the
     // result itself -- the palette is an authored look, the lighting is not.
     vec3 tint=mix(uFrame.styleCloudBase.rgb,uFrame.styleCloudMid.rgb,
                   smoothstep(.30,.80,n));
     tint=mix(tint,uFrame.styleCloudLit.rgb,clamp(lit*powder,0.0,1.0)*.7);
     vec3 cloud=(direct*0.09+ambient)*tint;
     float alpha=(1.0-exp(-tau))*mask;
     color=mix(color,cloud,clamp(alpha,0.0,1.0));
   }
 }
  // ---- volumetric cloud layer ------------------------------------------
  // Apply painterly grade to the sky atmosphere background first, then blend
  // the volumetric clouds over it. This maintains the smooth physical
  // volumetric lighting gradient on clouds while retaining the painterly
  // sky background.
  color = painterlyGradeSky(color, dir);

  if(!env && pc.passFlags.z > 0.001){
    vec2 cuv = vNdc * .5 + .5;
    vec4 cl = texture(uCloud, cuv);
    float s = clamp(pc.passFlags.z, 0.0, 1.0);
    float tr = mix(1.0, cl.a, s);
    color = cl.rgb * s + color * tr;
    cloudMask = 1.0 - tr;
  }
 if(!env&&sky){
   // ---- sun disc -----------------------------------------------------
   // Colour comes from sunRadiance (transmittance-coloured on the CPU), not
   // the fixed vec3(1,.86,.60) this used to add: that tint made a noon sun
   // the same warm yellow as a sunset one. kSunAngularRadius is the real
   // 0.53-degree disc; the old .009 rad was very nearly double that.
   const float kSunAngularRadius=0.00465;
   float ang=acos(clamp(mu,-1.0,1.0));
   float soft=mix(0.35,1.0,clamp(uFrame.styleSky0.w,0.0,1.0));
   float edge=1.0-smoothstep(kSunAngularRadius*(1.0-soft*0.55),
                             kSunAngularRadius*(1.0+soft*0.35),ang);
   // Limb darkening: a real disc is ~40% dimmer at the rim than at centre,
   // which is most of what stops it reading as a flat sticker.
   float r=clamp(ang/kSunAngularRadius,0.0,1.0);
   float limb=mix(1.0,sqrt(max(1.0-r*r,0.0)),0.62);
   // sunRadiance is ALREADY the ground-level radiance: it is
   // sunOuterScale x Chapman transmittance, computed on the CPU. Multiplying
   // it again by pc.sunDir.w (which IS sunOuterScale) and by a.transmittance
   // (the same path's extinction, counted a second time) put the disc about
   // 4000x over range. Everything past the first thousandth of the edge ramp
   // then clipped, so the "disc" was the smoothstep's outer bound rendered as
   // a hard 9-pixel square -- and normalising by luminance on top of that
   // crushed green and blue to exactly 0, making a sunset sun pure red.
   // Use the radiance as-is; sunDiscIntensity is the artistic multiplier.
   color+=uFrame.sunRadiance.rgb*uFrame.styleCloudMid.w*edge*limb*
          (1.0-cloudMask*0.97);
   // Aureole: the tight forward-scatter halo hugging the disc. The
   // atmosphere integral carries the broad glow already, but at 12 steps it
   // cannot resolve the first degree or so around the sun.
   float aureole=pow(max(mu,0.0),2200.0)*0.55+pow(max(mu,0.0),140.0)*0.05;
   color+=uFrame.sunRadiance.rgb*aureole*(1.0-cloudMask*0.85);

   // ---- moon ----------------------------------------------------------
   float mr=max(pc.passFlags.y,1e-4),ma=acos(clamp(dot(dir,toMoon),-1,1));
   float moonDisc=1.0-smoothstep(mr*.82,mr,ma);
   float mlimb=mix(1.0,sqrt(max(1.0-pow(clamp(ma/mr,0.0,1.0),2.0),0.0)),0.5);
   color+=vec3(.88,.92,1.0)*pc.moonDir.w*18.0*moonDisc*mlimb*a.transmittance*
          (1.0-cloudMask*0.97);
   // Halo. moonGlowIntensity has been packed into styleCloudBase.w and read
   // by no shader since the atmosphere rewrite -- an editor slider wired to
   // nothing, exactly like the fog day/night colours were.
   float halo=exp(-ma/max(mr*7.0,1e-3));
   color+=vec3(.72,.80,1.0)*pc.moonDir.w*halo*uFrame.styleCloudBase.w*
          a.transmittance*(1.0-cloudMask*0.9);

   // ---- stars ---------------------------------------------------------
   // The old test was hash(floor(dir*520)) > .99935: one cell either fully
   // on or fully off, so stars were all the same brightness, all the same
   // colour, and snapped between cells as the camera turned. This keeps the
   // sparse cell lookup but places the star INSIDE its cell and gives it a
   // magnitude and a colour, which is most of what makes a starfield read.
   // Milky way. starIntensity's doc comment has read "night starfield + milky
   // way" since the atmosphere rewrite while the shader only ever drew the
   // starfield. A broad, eroded band around a fixed great circle is enough to
   // read as one, and it costs two fbm taps on night pixels only.
   const vec3 kGalacticPole=normalize(vec3(0.35,0.62,-0.70));
   float band=1.0-abs(dot(dir,kGalacticPole));
   float mw=smoothstep(0.80,0.995,band);
   if(mw>0.001){
     float dust=fbm(dir*11.0)*0.7+fbm(dir*29.0)*0.3;
     float glow=mw*mw*smoothstep(0.25,0.75,dust);
     color+=mix(vec3(.62,.66,.86),vec3(.92,.88,.80),dust)*glow*
            uFrame.styleCloudLit.w*0.055*(1.0-sunUp)*pow(1.0-cloudMask,3.0);
   }
   vec3 cell=floor(dir*430.0);
   float pick=hash31(cell);
   if(pick>.9986){
     vec3 jitter=vec3(hash31(cell+1.7),hash31(cell+3.3),hash31(cell+7.1))-0.5;
     vec3 starDir=normalize((cell+0.5+jitter*0.9)/430.0);
     float d=acos(clamp(dot(dir,starDir),-1.0,1.0));
     float mag=mix(0.25,1.0,hash31(cell+11.9));
     float twinkle=0.75+0.25*sin(uFrame.miscParams.z*(1.7+pick*4.0)+pick*40.0);
     // Bluer/whiter or warmer, by spectral class.
     vec3 sc=mix(vec3(1.0,.86,.72),vec3(.78,.86,1.0),hash31(cell+17.3));
     float prof=exp(-d*d/(2.0*0.0016*0.0016));
     color+=sc*prof*mag*twinkle*uFrame.styleCloudLit.w*2.6*(1.0-sunUp)*
            pow(1.0-cloudMask,3.0);
   }
 }
 // The other half of removing the horizon seam. Terrain now dissolves all the
 // way instead of freezing at maxOpacity, but it still needs something to
 // dissolve INTO -- an unfogged sky just moves the discontinuity to the
 // skyline. Ground fog seen edge-on is optically very deep (a level ray never
 // leaves the layer), so this is a strong band at the horizon that thins fast
 // with view elevation, and thins again when the camera climbs above the mist.
 //
 // Env-map faces are excluded: surfaces sample the cubemap back as the color
 // their OWN fog scatters, so fogging it here would apply the medium twice.
 // The sky's radiance in this direction stands in for the skylight lighting
 // the fog; near the horizon (where tau is large enough to matter) that IS
 // the right color, and where it is wrong -- high overhead rays -- tau is
 // small enough that the source barely contributes.
 if(!env)
   color=fogSky(color,dir,color);
 outColor=vec4(max(color,vec3(0)),1);
}
