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
layout(location=4) in vec4 vTerrainParams;
layout(location=5) in float vGrassGroundOcclusion;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D uTextures[];
#include "frameData.glsl"
#include "skyModel.glsl"
#include "biomeLighting.glsl"
#include "fog.glsl"
layout(set=2,binding=0) uniform accelerationStructureEXT uTLAS;
layout(set=3,binding=0) uniform sampler2D uSSAO;
layout(set=4,binding=0) uniform samplerCube uEnvMap;
#include "cloudShadow.glsl"
layout(push_constant) uniform Push { mat4 model; uint textureIndex; } pc;
const uint NO_TEX=0xffffffffu;
#include "surfaceShadow.glsl"
#include "paintMaterial.glsl"
#include "snowMaterial.glsl"

float valueNoise(vec2 p){
 vec2 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);
 float a=paintHash13(vec3(i,1)),b=paintHash13(vec3(i+vec2(1,0),1));
 float c=paintHash13(vec3(i+vec2(0,1),1)),d=paintHash13(vec3(i+1.0,1));
 return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
}
float fbm(vec2 p){return valueNoise(p)*0.58+valueNoise(p*2.17+9.2)*0.29+
 valueNoise(p*4.31-5.7)*0.13;}

uint layerTex(int i){if(i==0)return uFrame.terrainTexA.x;if(i==1)return uFrame.terrainTexA.w;
 if(i==2)return uFrame.terrainTexB.z;if(i==3)return uFrame.terrainTexC.y;return uFrame.terrainTexD.x;}
uint layerNormalTex(int i){if(i==0)return uFrame.terrainTexA.y;if(i==1)return uFrame.terrainTexB.x;
 if(i==2)return uFrame.terrainTexB.w;if(i==3)return uFrame.terrainTexC.z;return uFrame.terrainTexD.y;}
uint layerRoughTex(int i){if(i==0)return uFrame.terrainTexA.z;if(i==1)return uFrame.terrainTexB.y;
 if(i==2)return uFrame.terrainTexC.x;if(i==3)return uFrame.terrainTexC.w;return uFrame.terrainTexD.z;}
float layerTiling(int i){if(i==0)return uFrame.terrainTiling0.x;if(i==1)return uFrame.terrainTiling0.y;
 if(i==2)return uFrame.terrainTiling0.z;if(i==3)return uFrame.terrainTiling0.w;return uFrame.terrainTiling1.x;}

// The painted palette for layer i: a two-tone mottle, optionally modulated
// by the photo texture's LUMINANCE only (overlayStrength). This is the old
// stylised ground; realismParams.x blends from it to the photo albedo.
vec3 paintLayer(int i,vec2 xz,vec3 photo,bool hasPhoto){
 float scale=max(uFrame.terrainPaintLit[i].w,0.25);
 float m=fbm(xz/scale)-0.5;
 vec3 c=mix(uFrame.terrainPaintShade[i].rgb,uFrame.terrainPaintLit[i].rgb,
            clamp(0.58+m*0.34,0.0,1.0));
 float strength=uFrame.terrainPaintShade[i].w;
 if(strength>0.001&&hasPhoto){
   float y=max(dot(photo,vec3(.2126,.7152,.0722)),.08);
   c*=mix(vec3(1),photo/y,strength);
 }
 return c;
}

// ---------------------------------------------------------------------------
// Photographic layer sampling
// ---------------------------------------------------------------------------
// Everything below uses textureGrad with derivatives taken ONCE at the top
// of main(): layers are sampled only where their weight is non-zero, and
// implicit derivatives inside that divergent branch are undefined.
struct LayerTap { vec3 albedo; vec3 n; float rough; float height; };
uint layerHeightTex(int i){return i<4?uFrame.terrainHeightTex[i]:uFrame.terrainHeightTexExtra.x;}
float layerRelief(int i){return i<4?uFrame.terrainReliefDepth0[i]:uFrame.terrainReliefDepth1.x;}
float tapHeight(uint tex,vec2 uv,vec2 gx,vec2 gy){
 if(tex==NO_TEX)return .5;
 return textureGrad(uTextures[nonuniformEXT(tex)],uv,gx,gy).r;
}

// Bounded parallax occlusion, with a linear refinement of the crossing.
// Relief is authored in metres, independent of texture resolution/tiling.
// Use explicit gradients throughout the divergent march. Fade before grazing
// angles, cliffs and distant pixels: a heightfield cannot change silhouettes.
vec2 reliefUV(uint tex,vec2 uv,vec2 gx,vec2 gy,vec2 ray,float amount){
 if(tex==NO_TEX||amount<.001)return uv;
 ray*=amount;
 const int steps=12;
 vec2 delta=ray/float(steps),p=uv+ray*.5,previous=p;
 float depth=0.0,oldDepth=0.0;
 float surface=1.0-tapHeight(tex,p,gx,gy),oldSurface=surface;
 for(int j=0;j<steps;++j){
   if(depth>=surface)break;
   previous=p;oldDepth=depth;oldSurface=surface;
   p-=delta;depth+=1.0/float(steps);
   surface=1.0-tapHeight(tex,p,gx,gy);
 }
 float before=oldSurface-oldDepth,after=depth-surface;
 return mix(previous,p,clamp(before/max(before+after,1e-5),0.0,1.0));
}

// Photo albedo re-coloured toward the authored palette. A scanned texture
// carries the colour of wherever it was photographed -- the stock meadow
// here is a grey, late-season moss -- so on its own it cannot be art
// directed. Dividing by the texture's MEAN (its last mip level, i.e. the
// GPU's own box average) and multiplying by the palette's mean keeps every
// bit of the photo's local contrast and structure but moves its average hue
// and value to what the scenery asked for. overlayStrength is the dial:
// 0 = the photo as scanned, 1 = fully palette-matched.
vec3 paletteMatch(int i,vec3 photo){
 float strength=clamp(uFrame.terrainPaintShade[i].w,0.0,1.0);
 uint tex=layerTex(i);
 if(strength<=0.001||tex==NO_TEX)return photo;
 vec3 mean=textureLod(uTextures[nonuniformEXT(tex)],vec2(0.5),16.0).rgb;
 vec3 target=mix(uFrame.terrainPaintShade[i].rgb,uFrame.terrainPaintLit[i].rgb,0.58);
 vec3 ratio=target/max(mean,vec3(0.01));
 // Bounded so a near-black channel in the mean cannot explode.
 return photo*mix(vec3(1.0),clamp(ratio,vec3(0.2),vec3(5.0)),strength);
}

vec3 tapNormal(uint tex,vec2 uv,vec2 gx,vec2 gy){
 if(tex==NO_TEX)return vec3(0,0,1);
 return textureGrad(uTextures[nonuniformEXT(tex)],uv,gx,gy).xyz*2.0-1.0;
}
float tapRough(uint tex,vec2 uv,vec2 gx,vec2 gy){
 if(tex==NO_TEX)return -1.0;
 return textureGrad(uTextures[nonuniformEXT(tex)],uv,gx,gy).r;
}

// Top-down projection with anti-tiling. One photo tiled every few metres
// repeats visibly across a whole valley; a second tap of the SAME texture,
// rotated ~37 degrees and rescaled, is blended in through a low-frequency
// mask so no two neighbouring tiles look alike. The mask is pushed by the
// two taps' height difference, so the seam between them
// follows stones and tufts rather than drifting across them as a soft
// cross-fade. Normals come out in world space (whiteout-blended onto N).
LayerTap sampleTopDown(int i,vec3 wp,vec3 N,vec3 dpx,vec3 dpy){
 LayerTap t;
 float tile=max(layerTiling(i),0.5);
 uint at=layerTex(i),nt=layerNormalTex(i),rt=layerRoughTex(i);
 vec2 uvA=wp.xz/tile,gxA=dpx.xz/tile,gyA=dpy.xz/tile;
 const float c=0.7986,s=0.6018; // cos/sin 37 degrees
 mat2 R=mat2(c,s,-s,c)*1.27;
 vec2 uvB=R*uvA+vec2(0.37,0.71),gxB=R*gxA,gyB=R*gyA;
 uint ht=layerHeightTex(i);
 vec3 view=uFrame.camPosWS.xyz-wp;
 float amount=(1.0-smoothstep(10.0,38.0,length(view)))*
              smoothstep(.12,.30,normalize(view).y)*smoothstep(.65,.9,N.y)*
              uFrame.realismParams.x;
 vec2 ray=view.xz/max(view.y,.18*length(view))*clamp(layerRelief(i),0.0,.2)/tile;
 uvA=reliefUV(ht,uvA,gxA,gyA,ray,amount);
 uvB=reliefUV(ht,uvB,gxB,gyB,R*ray,amount);
 vec3 aA=textureGrad(uTextures[nonuniformEXT(at)],uvA,gxA,gyA).rgb;
 vec3 aB=textureGrad(uTextures[nonuniformEXT(at)],uvB,gxB,gyB).rgb;
 float hA=tapHeight(ht,uvA,gxA,gyA),hB=tapHeight(ht,uvB,gxB,gyB);
 float m=valueNoise(wp.xz/(tile*2.3)+float(i)*7.3)*.65+
         valueNoise(wp.xz/(tile*0.9)-float(i)*3.1)*.35;
 m=clamp((m-0.5)*3.2+(hB-hA)*2.5+0.5,0.0,1.0);
 // Woodland uses scanned litter and small leaves. A broad crossfade made
 // those features look like two translucent photos printed over each other.
 // Keep a narrow, height-driven seam while preserving the other profiles.
 if(uFrame.terrainTiling1.y>.5)m=smoothstep(.35,.65,m);
 t.albedo=mix(aA,aB,m);
 t.height=mix(hA,hB,m);
 vec3 nA=tapNormal(nt,uvA,gxA,gyA),nB=tapNormal(nt,uvB,gxB,gyB);
 // Tap B's tangent frame is rotated by R; undo it so its bumps lean the
 // same way in world space as the geometry they sit on.
 nB.xy=transpose(mat2(c,s,-s,c))*nB.xy;
 vec3 tn=normalize(mix(nA,nB,m));
 t.n=normalize(vec3(tn.x+N.x,abs(tn.z)*N.y,tn.y+N.z));
 float rA=tapRough(rt,uvA,gxA,gyA),rB=tapRough(rt,uvB,gxB,gyB);
 t.rough=rA<0.0?-1.0:mix(rA,rB,m);
 return t;
}

// Triplanar for cliffs and scree. Top-down UVs stretch into vertical streaks
// wherever the slope passes ~50 degrees, which is exactly where rock shows;
// projecting from all three axes and blending by the normal keeps the
// texel density constant on any face.
LayerTap sampleTriplanar(int i,vec3 wp,vec3 N,vec3 dpx,vec3 dpy,float strength){
 LayerTap top=sampleTopDown(i,wp,N,dpx,dpy);
 vec3 bw=pow(abs(N),vec3(4.0));bw/=max(bw.x+bw.y+bw.z,1e-4);
 bw=mix(vec3(0,1,0),bw,strength);
 if(bw.y>0.985)return top;
 float tile=max(layerTiling(i),0.5);
 uint at=layerTex(i),nt=layerNormalTex(i),rt=layerRoughTex(i);
 vec2 uvX=wp.zy/tile,gxX=dpx.zy/tile,gyX=dpy.zy/tile;
 vec2 uvZ=wp.xy/tile+vec2(0.5,0.25),gxZ=dpx.xy/tile,gyZ=dpy.xy/tile;
 vec3 aX=textureGrad(uTextures[nonuniformEXT(at)],uvX,gxX,gyX).rgb;
 vec3 aZ=textureGrad(uTextures[nonuniformEXT(at)],uvZ,gxZ,gyZ).rgb;
 vec3 tX=tapNormal(nt,uvX,gxX,gyX),tZ=tapNormal(nt,uvZ,gxZ,gyZ);
 // Whiteout blend per axis (Golus), then swizzle each back to world.
 vec3 nX=vec3(abs(tX.z)*N.x,tX.y+N.y,tX.x+N.z);
 vec3 nZ=vec3(tZ.x+N.x,tZ.y+N.y,abs(tZ.z)*N.z);
 LayerTap t;
 t.albedo=aX*bw.x+top.albedo*bw.y+aZ*bw.z;
 t.n=normalize(nX*bw.x+top.n*bw.y+nZ*bw.z);
 float rX=tapRough(rt,uvX,gxX,gyX),rZ=tapRough(rt,uvZ,gxZ,gyZ);
 t.rough=top.rough<0.0?-1.0:rX*bw.x+top.rough*bw.y+rZ*bw.z;
 uint ht=layerHeightTex(i);
 t.height=tapHeight(ht,uvX,gxX,gyX)*bw.x+top.height*bw.y+tapHeight(ht,uvZ,gxZ,gyZ)*bw.z;
 return t;
}

void main(){
 vec3 smoothN=normalize(vNormalWS);
 // All derivatives up front, in uniform control flow.
 vec3 dpx=dFdx(vWorldPos),dpy=dFdy(vWorldPos);
 float wF=vTerrainParams.z,wM=vTerrainParams.w,wMead=clamp(1.0-wF-wM,0.0,1.0);
 float slope=1.0-clamp(smoothN.y,0.0,1.0);
 float rock=clamp(smoothstep(uFrame.terrainMat2.x,uFrame.terrainMat2.y,slope)*
   mix(.72,1.16,vTerrainParams.y)+wM*.18,0.0,1.0);
 float scree=rock*clamp(wM*1.2,0.0,1.0);
 float dirt=smoothstep(.50,.16,vTerrainParams.x)*(1.0-rock)*uFrame.terrainMat2.z;
 float weights[5];weights[0]=wMead*(1.0-rock)*(1.0-dirt);
 weights[1]=wF*(1.0-rock)*(1.0-dirt);weights[2]=dirt;
 weights[3]=rock-scree;weights[4]=scree;
 if(uFrame.terrainTiling1.y>.5){
   // Coverage comes from the same world-space layout as collision and scatter.
   // Slot 4 is wet soil in this profile; other sceneries retain their scree.
   float track=clamp(vUV.x,0.0,1.0);
   // Vertex coverage shares the physical road layout. Sub-metre breakup here
   // survives coarse terrain triangles: moss tongues and loose gravel follow
   // an irregular shoulder rather than a straight interpolated contour.
   float edgeNoise=(valueNoise(vWorldPos.xz*1.7)-.5)*.9+
                   (valueNoise(vWorldPos.xz*5.3+19.7)-.5)*.35;
   track=clamp(track+edgeNoise*4.0*track*(1.0-track),0.0,1.0);
   rock=max(rock,vTerrainParams.y);
   float mud=smoothstep(.68,.95,vUV.y)*(1.0-smoothstep(.0,1.5,vWorldPos.y));
   float litter=smoothstep(.12,.62,wF);
   litter=max(litter,.42*smoothstep(.48,.70,fbm(vWorldPos.xz*.73+43.1))*(1.0-track));
   float bare=.12+.45*smoothstep(.42,.66,fbm(vWorldPos.xz*.047));
   float soil=max(track,bare*(1.0-litter));
   weights[4]=mud;
   weights[3]=rock*(1.0-mud);
   weights[2]=soil*(1.0-rock)*(1.0-mud);
   float remaining=(1.0-soil)*(1.0-rock)*(1.0-mud);
   weights[1]=litter*remaining;
   weights[0]=(1.0-litter)*remaining;
 }
 float total=max(weights[0]+weights[1]+weights[2]+weights[3]+weights[4],.001);
 float photo=uFrame.realismParams.x;

 // Sample every contributing layer once, then blend by HEIGHT rather than
 // by weight alone: where two materials meet, the one whose texel is
 // taller (from the height map -- pebbles, tuft tips) wins
 // outright, the way grass fills the gaps between stones instead of
 // cross-fading over them like a double exposure.
 LayerTap taps[5];
 float hb[5];
 float hmax=-1e9;
 for(int i=0;i<5;i++){
   weights[i]/=total;
   taps[i].albedo=vec3(0);taps[i].n=smoothN;taps[i].rough=-1.0;taps[i].height=.5;hb[i]=-1e9;
   if(weights[i]<0.004)continue;
   if(layerTex(i)!=NO_TEX){
     taps[i]=(i>=3)?sampleTriplanar(i,vWorldPos,smoothN,dpx,dpy,uFrame.realismParams.w)
                   :sampleTopDown(i,vWorldPos,smoothN,dpx,dpy);
   }
   float h=taps[i].height;
   hb[i]=weights[i]+h*0.45;
   hmax=max(hmax,hb[i]);
 }
 const float kBlendDepth=0.18;
 float hsum=0.0;
 for(int i=0;i<5;i++){
   float w=max(hb[i]-(hmax-kBlendDepth),0.0)*step(0.004,weights[i]);
   hb[i]=w;hsum+=w;
 }
 vec3 albedo=vec3(0),mappedNormal=vec3(0);float mappedNormalWeight=0.0;
 float reliefHeight=0.0,heightWeight=0.0;
 float mappedRoughness=0.0,mappedRoughnessWeight=0.0;
 for(int i=0;i<5;i++){
   // Weighted mix of plain and height-sharpened weights: photo mode gets
   // the crisp transitions; the painted mode keeps its soft wash.
   float w=mix(weights[i],hb[i]/max(hsum,1e-4),photo);
   if(w<=0.0)continue;
   bool hasPhoto=layerTex(i)!=NO_TEX;
   vec3 layerAlbedo=paletteMatch(i,taps[i].albedo);
   if(!hasPhoto||photo<0.999)
     layerAlbedo=mix(paintLayer(i,vWorldPos.xz,taps[i].albedo,hasPhoto),
                     layerAlbedo,hasPhoto?photo:0.0);
   albedo+=layerAlbedo*w;
   if(layerHeightTex(i)!=NO_TEX){reliefHeight+=taps[i].height*w;heightWeight+=w;}
   if(layerNormalTex(i)!=NO_TEX){mappedNormal+=taps[i].n*w;mappedNormalWeight+=w;}
   if(taps[i].rough>=0.0){mappedRoughness+=taps[i].rough*w;mappedRoughnessWeight+=w;}
 }
 // A broad procedural layer alone collapses to a flat fill at ground-level
 // camera distances. Two restrained, differently oriented frequencies supply
 // aggregate, grit and damp patches without imposing a themed pattern.
 // uFrame.terrainMat2.w = antiTileStrength (default 0.35)
 // uFrame.terrainMat3: x=biomeTintEnabled (dead), y=biomeTintIntensity (dead), z=macroVariationStrength (default 0.15), w=rockDetailStrength (default 0.3)
 float aggregate=fbm(vWorldPos.xz*0.82)-0.5;
 float grit=valueNoise(vWorldPos.xz*5.7+aggregate*2.1)-0.5;
 float damp=smoothstep(0.20,0.48,fbm(vWorldPos.xz*0.115+vec2(17.0,-9.0)));
 albedo*=1.0+(aggregate*1.6+grit*0.5)*uFrame.terrainMat3.z*(1.0-0.6*photo);
 albedo=mix(albedo,albedo*vec3(.68,.73,.77),damp*.22*(1.0-rock));
 // Macro variation at 50-400 m. Even an anti-tiled photo layer averages to
 // one flat colour at distance (the mips converge on its mean), so a whole
 // hillside turns a uniform green. Real ground drifts in hue and value at
 // this scale -- drier, lusher, sun-bleached patches -- and that drift is
 // what the eye uses to judge distance across a landscape.
 float macro=fbm(vWorldPos.xz*0.0035+vec2(3.1,7.7))-0.5;
 float macro2=fbm(vWorldPos.xz*0.019-vec2(11.3,2.9))-0.5;
 vec3 dry=albedo*vec3(1.16,1.05,0.82),lush=albedo*vec3(0.86,1.02,0.90);
 albedo=mix(albedo,macro>0.0?dry:lush,clamp(abs(macro)*1.3+macro2*0.4,0.0,1.0)*
            photo*(1.0-rock*0.7));
 albedo*=1.0+macro2*0.22*photo;
 vec3 gold=vec3(dot(albedo,vec3(.30,.59,.11)))*vec3(1.18,.82,.27);
 albedo=mix(albedo,gold,wF*uFrame.stylePaint1.y*.55);
 float footprint=length(fwidth(vWorldPos.xz));
 // The old high-frequency sine produced parallel yellow worms across
 // meadows. Broad isotropic variation reads as soil, without a preferred axis.
 float strokes=(fbm(vWorldPos.xz*.6)-.5)*
               (1.0-smoothstep(.1,.8,footprint));
 albedo*=1.0+strokes*(weights[0]+weights[1])*uFrame.terrainMat2.w*.12*(1.0-photo);
 float edge=0.0;for(int i=0;i<5;i++)edge+=length(vec2(dFdx(weights[i]),dFdy(weights[i])));
 albedo*=1.0-clamp(edge*uFrame.stylePaint0.w,0.0,.25);
 float paper=valueNoise(vWorldPos.xz*2.0)-.5;
 albedo*=1.0+paper*uFrame.stylePaint0.z;
 vec3 facet=normalize(cross(dpx,dpy));
 if(dot(facet,smoothN)<0.0)facet=-facet;
 vec3 N=normalize(mix(smoothN,facet,clamp(wM*rock*uFrame.stylePaint1.x*(uFrame.terrainMat3.w/0.3),0.0,1.0)));
 if(mappedNormalWeight>0.001){
   vec3 detailNormal=normalize(mappedNormal/max(mappedNormalWeight,0.001));
   // Photo mode takes the texture's normal at full strength: the whiteout
   // blend above already carries the geometric normal inside it. Distance
   // fade keeps far slopes from sparkling once a texel covers many pixels.
   float far=smoothstep(60.0,260.0,vViewZ);
   float k=mix(.82,mix(1.0,.55,far),photo);
   N=normalize(mix(N,detailNormal,clamp(mappedNormalWeight*k,0.0,1.0)));
 }
 vec3 L=normalize(-uFrame.lightDir.xyz),V=normalize(uFrame.camPosWS.xyz-vWorldPos);
 // Fog before the shadow rays, so fully fogged terrain skips them entirely.
 vec3 viewDirWS=normalize(vWorldPos-uFrame.camPosWS.xyz);
 vec3 skyAhead=textureLod(uEnvMap,viewDirWS,1.0).rgb;
 vec3 skyAbove=textureLod(uEnvMap,vec3(viewDirWS.x,max(viewDirWS.y,.25),viewDirWS.z),2.0).rgb;
 FogSample fog=fogAlongView(vWorldPos,biomeFogDensityMult(wF,wM),
   mountainAerialPerspective(max(uFrame.camPosWS.y-vWorldPos.y,0.0),wM),
   biomeFogTint(vec3(1.0),wF,wM),skyAhead,skyAbove);
 float vis=1.0;if(dot(N,L)>0.0&&fog.opacity<.9)vis=shadowVis(vWorldPos+smoothN*.02,L);
 vis=mix(1.0,vis,uFrame.lightParams.y);
 vis*=cloudTransmission(vWorldPos);
 float screenAO=texture(uSSAO,gl_FragCoord.xy/vec2(textureSize(uSSAO,0))).r;
 float ssao=screenAO;
 // Screen-space AO cannot see relief that only exists in the texture. Use
 // the scanned height to reduce ambient light in those micro-crevices;
 // albedo darkness alone would incorrectly treat black soil as occluded.
 float cavity=smoothstep(.25,.55,reliefHeight/max(heightWeight,1e-4));
 ssao*=mix(1.0,mix(.72,1.0,cavity),photo*heightWeight);
 vec2 sunXZ=normalize(-L.xz+1e-5);float directMult=forestCanopyDirect(vWorldPos,wF,uFrame.miscParams.z,sunXZ)*mountainDirectMultiplier(wM);
 vec4 amb=biomeAmbient(wMead,wF,wM);
 // The placement mask only gates contact enhancement. Actual depth-prepass
 // occluders define the pockets: uniform coverage darkening looked like a
 // different soil material rather than grass growing out of it.
 float grassRange=uFrame.postAAParams.y;
 float grassCover=step(.001,grassRange)*clamp(vGrassGroundOcclusion,0.0,1.0)*
   (1.0-smoothstep(grassRange*.65,max(grassRange,1.0),length(uFrame.camPosWS.xyz-vWorldPos)));
 // Sparse AO samples can miss a thin blade at one pixel while its neighbours
 // are occluded. Raising that isolated value to a power made tiny soil flecks
 // look sunlit inside a shaded clump. Extend nearby contacts over at most
 // four centimetres, bounded in pixels, before amplifying them. Large gaps
 // and ground outside the grass placement mask keep their original lighting.
 float grassAO=screenAO;
 if(grassCover>.001){
   ivec2 aoSize=textureSize(uSSAO,0);
   vec2 aoPixel=1.0/vec2(aoSize);
   vec2 aoUV=gl_FragCoord.xy*aoPixel;
   vec2 contactPixels=clamp(vec2(.04)/max(vec2(length(dpx),length(dpy)),vec2(.001)),
                            vec2(1.0),vec2(12.0));
   float nearbyAO=screenAO;
   for(int y=-1;y<=1;y++)for(int x=-1;x<=1;x++){
     vec2 tapUV=clamp(aoUV+vec2(x,y)*contactPixels*aoPixel,
                      aoPixel*.5,vec2(1.0)-aoPixel*.5);
     nearbyAO=min(nearbyAO,texture(uSSAO,tapUV).r);
   }
   grassAO=nearbyAO;
 }
 float grassPocket=grassCover*(1.0-pow(clamp(grassAO,0.0,1.0),6.0));
 // Preserve the soil's open-ground lighting between roots. Occluded pockets
 // also receive less direct fill; ambient multibounce alone washed them out.
 float grassSunTransmission=mix(1.0,.16,grassPocket);
 float grassSkyTransmission=mix(1.0,.10,grassPocket);
 directMult*=grassSunTransmission;
 float terrainRoughness=clamp(.91-grit*.10-damp*.18-rock*.16,.48,1.0);
 if(mappedRoughnessWeight>0.001)
   terrainRoughness=mix(terrainRoughness,
     mappedRoughness/max(mappedRoughnessWeight,0.001),
     clamp(mappedRoughnessWeight,0.0,1.0));
 // Sheltered soil is diffuse. Glossy texels in scanned ground otherwise leave
 // small specular flashes even where the grass has blocked most of the light.
 terrainRoughness=mix(terrainRoughness,max(terrainRoughness,.96),grassPocket);
 applySnow(vWorldPos,smoothN,1.0,albedo,N,terrainRoughness);
 terrainRoughness=paintFilterRoughness(terrainRoughness,paintNormalVariance(N));
 vec3 irr=paintDiffuseEnvironment(uEnvMap,N)*grassSkyTransmission;
 vec3 color=physicalPaintSurface(albedo,N,V,L,terrainRoughness,0.0,vis,ssao,irr,directMult,amb.w)*amb.rgb;
 vec3 R=reflect(-V,N);
 vec3 envSpec=textureLod(uEnvMap,R,terrainRoughness*uFrame.iblParams.y).rgb*
              paintHorizonOcclusion(R,smoothN);
 color+=physicalEnvironmentSpecular(albedo,N,V,terrainRoughness,0.0,ssao,envSpec)*
        uFrame.iblParams.z*uFrame.iblParams.w*grassSkyTransmission;
 color+=physicalPointLights(albedo,N,V,vWorldPos,terrainRoughness,0.0)*grassSunTransmission;
 // Camera transport is applied after opaque shading.
 int dbg=int(uFrame.miscParams.x+.5);if(dbg==1)color=albedo;else if(dbg==2)color=N*.5+.5;
 else if(dbg==3)color=vec3(fog.opacity);else if(dbg==4)color=vec3(ssao*grassSkyTransmission);else if(dbg==5)color=vec3(vis);
 else if(dbg==7)color=vec3(wMead,wF,wM);else if(dbg==6)color=(any(isnan(color))||any(isinf(color)))?vec3(1,0,1):vec3(0);
 outColor=vec4(max(color,vec3(0)),1);
}
