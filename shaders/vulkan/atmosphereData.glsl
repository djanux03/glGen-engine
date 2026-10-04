#ifndef ATMOSPHERE_DATA_GLSL
#define ATMOSPHERE_DATA_GLSL
#ifndef ATMOSPHERE_SET
#define ATMOSPHERE_SET 0
#endif
layout(set=ATMOSPHERE_SET,binding=0,std140) uniform AtmosphereData {
    mat4 invViewProj,prevViewProj,prevView,invSurfaceViewProj,view;
    vec4 camera,previousCamera,grid,ground,groundAlbedo,dust,dustAlbedo;
    vec4 history,controls,terrain,terrainField;
    vec4 pointControls[4];
    vec4 reference;
    vec4 artistic;
    mat4 viewProj;
    vec4 valley;
} uAtmosphere;
layout(set=ATMOSPHERE_SET,binding=1) uniform sampler3D uFogCurrent;
layout(set=ATMOSPHERE_SET,binding=2) uniform sampler3D uFogHistory;
layout(set=ATMOSPHERE_SET,binding=3) uniform sampler3D uFogIntegrated;
layout(set=ATMOSPHERE_SET,binding=8) uniform sampler3D uFogVisibility;
layout(set=ATMOSPHERE_SET,binding=9) uniform sampler2D uUnfoggedScene;
layout(set=ATMOSPHERE_SET,binding=10) uniform sampler2D uFogDepth;
layout(set=ATMOSPHERE_SET,binding=11) uniform sampler2D uFogTerrain;
layout(set=ATMOSPHERE_SET,binding=12) uniform sampler3D uFogFiltered;
float fogBoundary(float i) {
    return i<=0.0 ? 0.0 : .05*pow(uAtmosphere.grid.w/.05,(i-1.0)/(uAtmosphere.grid.z-1.0));
}
float fogSlice(float distance) {
    return distance<=.05 ? distance/.05 : 1.0+log(distance/.05)/log(uAtmosphere.grid.w/.05)*(uAtmosphere.grid.z-1.0);
}
vec3 fogRay(vec2 uv) {
    vec4 p=uAtmosphere.invViewProj*vec4(uv*2.0-1.0,1,1);
    return normalize(p.xyz/p.w-uAtmosphere.camera.xyz);
}
// Slice depths are camera-plane distances, consistent with reprojection.
vec3 fogPosition(vec2 uv,float depth) {
    vec3 direction=fogRay(uv);
    return uAtmosphere.camera.xyz+direction*depth/max(-(uAtmosphere.view*vec4(direction,0)).z,.001);
}
float fogSegmentWeight(float sigma,float distance) {
    float tau=max(sigma,0.0)*max(distance,0.0);
    return tau<1e-3 ? distance*(1.0-tau*.5+tau*tau/6.0) : (1.0-exp(-tau))/sigma;
}
float fogHG(float mu,float g) {
    g=clamp(g,-.95,.95);
    float d=max(1e-6,1.0+g*g-2.0*g*clamp(mu,-1.0,1.0));
    return (1.0-g*g)/(12.566370614359172*d*sqrt(d));
}
float fogHeightAbove(float y,float dy,float distance,float falloff) {
    float x=falloff*abs(dy)*distance;
    float weight=x<1e-3 ? distance*(1.0-x*.5+x*x/6.0) : (1.0-exp(-x))/(falloff*abs(dy));
    return exp(-falloff*max(min(y,y+dy*distance),0.0))*weight;
}
float fogHeightIntegral(float y,float dy,float distance,float density,float falloff,float reference) {
    distance=max(distance,0.0);density=max(density,0.0);falloff=max(falloff,0.0);
    if(falloff==0.0||distance==0.0) return density*distance;
    y-=reference;float end=y+dy*distance;
    if(y<=0.0&&end<=0.0) return density*distance;
    if(y>=0.0&&end>=0.0) return density*fogHeightAbove(y,dy,distance,falloff);
    float crossing=clamp(-y/dy,0.0,distance);
    return density*(y<0.0 ? crossing+fogHeightAbove(0.0,dy,distance-crossing,falloff)
                         : fogHeightAbove(y,dy,crossing,falloff)+distance-crossing);
}

#endif
