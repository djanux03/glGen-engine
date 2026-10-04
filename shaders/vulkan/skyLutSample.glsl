#ifndef SKY_LUT_SET
#define SKY_LUT_SET 0
#endif
layout(set=SKY_LUT_SET,binding=13) uniform sampler2D uSkyTransmittance;
layout(set=SKY_LUT_SET,binding=14) uniform sampler2D uSkyMultiple;
layout(set=SKY_LUT_SET,binding=15) uniform sampler2D uSkyView;
layout(set=SKY_LUT_SET,binding=16) uniform sampler2D uSkyViewTransmission;
layout(set=SKY_LUT_SET,binding=17) uniform sampler3D uSkyAerial;
layout(set=SKY_LUT_SET,binding=18) uniform sampler3D uSkyAerialTransmission;
#include "skyLutModel.glsl"
vec3 skyLightTransmission(vec3 p,vec3 toLight) {
 vec2 hit=skySphere(p,toLight,SKY_BOTTOM);
 if(hit.x>0) return vec3(0);
 return textureLod(uSkyTransmittance,skySubUv(skyTransUv(length(p),dot(normalize(p),toLight)),vec2(256,64)),0).rgb;
}
vec3 skyMultiple(vec3 p,vec3 toLight) {
 vec2 uv=vec2(dot(normalize(p),toLight)*.5+.5,(length(p)-SKY_BOTTOM)/(SKY_TOP-SKY_BOTTOM));
 return textureLod(uSkyMultiple,skySubUv(uv,vec2(32)),0).rgb;
}
// Aerial depth is camera-plane metres, quadratically distributed. Slice zero
// is exactly vacuum so near objects never receive half a first slice of fog.
void skySampleAerial(vec2 uv,float depth,out vec3 scattering,out vec3 transmission) {
 float z=sqrt(clamp(depth/32000.0,0,1));vec3 coord=vec3(uv,(z*31+.5)/32);
 scattering=textureLod(uSkyAerial,coord,0).rgb;
 transmission=textureLod(uSkyAerialTransmission,coord,0).rgb;
}
