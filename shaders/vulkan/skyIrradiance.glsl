#ifndef SKY_IRRADIANCE_GLSL
#define SKY_IRRADIANCE_GLSL
#ifndef SKY_ENVIRONMENT_SET
#define SKY_ENVIRONMENT_SET 4
#endif
layout(set=SKY_ENVIRONMENT_SET,binding=2,std430) readonly buffer SkyIrradiance {vec4 coefficients[16];} uSkySH;
#include "skySHBasis.glsl"
vec3 skyDiffuseIrradiance(vec3 N) {
 float b[9];skySHBasis(normalize(N),b);vec3 irradiance=vec3(0);
 for(int i=0;i<9;++i)irradiance+=uSkySH.coefficients[i].rgb*b[i]*(i==0?1.0:i<4?2.0/3.0:.25);
 // Band truncation can ring below zero around a narrow bright horizon.
 return max(irradiance,vec3(0));
}
vec3 skyMeanRadiance() {return max(uSkySH.coefficients[0].rgb*.2820947918,vec3(0));}
#endif
