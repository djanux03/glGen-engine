// Hillaire 2020 / Bruneton 2017 parameterization, adapted to kilometres.
// Reference: sebh/UnrealEngineSkyAtmosphere (MIT, Epic Games 2020).
const float SKY_PI=3.14159265359, SKY_BOTTOM=6371.0, SKY_TOP=6451.0;
vec2 skySphere(vec3 p,vec3 d,float radius) {
 float b=dot(p,d),c=(length(p)-radius)*(length(p)+radius),disc=b*b-c;
 if(disc<0) return vec2(1e9,-1);float s=sqrt(disc);return vec2(-b-s,-b+s);
}
vec3 skyPlanet(float worldY) {return vec3(0,SKY_BOTTOM+clamp((worldY+4)*.001,.002,40.0),0);}
vec3 skyExtinction(float h,float haze) {
 float r=exp(-max(h,0)/8.5),m=exp(-max(h,0)/1.2),o=max(0,1-abs(h-25)/15);
 return vec3(.005802,.013558,.0331)*r+vec3((.002+.022*haze*haze)*1.11*m)+vec3(.00065,.001881,.000085)*o;
}
vec3 skyScattering(float h,float haze) {
 return vec3(.005802,.013558,.0331)*exp(-max(h,0)/8.5)+vec3(.002+.022*haze*haze)*exp(-max(h,0)/1.2);
}
vec3 skyPhaseScattering(float h,float haze,float mu) {
 float g=.76,q=max(1e-6,1+g*g-2*g*mu);
 float mie=3*(1-g*g)*(1+mu*mu)/(8*SKY_PI*(2+g*g)*q*sqrt(q));
 return vec3(.005802,.013558,.0331)*exp(-max(h,0)/8.5)*(3*(1+mu*mu)/(16*SKY_PI))
      +vec3(.002+.022*haze*haze)*exp(-max(h,0)/1.2)*mie;
}
vec2 skyTransParams(vec2 uv) {
 float H=sqrt((SKY_TOP-SKY_BOTTOM)*(SKY_TOP+SKY_BOTTOM)),rho=H*uv.y;
 float r=sqrt(rho*rho+SKY_BOTTOM*SKY_BOTTOM),distance=mix(SKY_TOP-r,rho+H,uv.x);
 return vec2(r,distance>1e-6?clamp((H*H-rho*rho-distance*distance)/(2*r*distance),-1,1):1);
}
vec2 skyTransUv(float r,float mu) {
 float H=sqrt((SKY_TOP-SKY_BOTTOM)*(SKY_TOP+SKY_BOTTOM));
 float rho=sqrt(max(0,(r-SKY_BOTTOM)*(r+SKY_BOTTOM)));
 float distance=max(0,-r*mu+sqrt(max(0,r*r*(mu*mu-1)+SKY_TOP*SKY_TOP)));
 return vec2((distance-(SKY_TOP-r))/max(1e-5,rho+H-(SKY_TOP-r)),rho/H);
}
// Exact texel-center convention: LUT domain endpoints are the first/last
// centres. Using normalized coordinates directly shifts the horizon seam.
vec2 skySubUv(vec2 unit,vec2 size) {return (clamp(unit,0,1)*(size-1)+.5)/size;}
vec2 skyUnitUv(vec2 uv,vec2 size) {return clamp((uv*size-.5)/(size-1),0,1);}
float skyHorizon(float radius) {return acos(-sqrt(max(0,(radius-SKY_BOTTOM)*(radius+SKY_BOTTOM)))/radius);}
vec2 skyViewParams(vec2 uv,float radius) {
 uv=skyUnitUv(uv,vec2(192,108));float horizon=skyHorizon(radius),beta=SKY_PI-horizon;
 float theta=uv.y<.5?horizon*(1-pow(1-uv.y*2,2)):horizon+beta*pow(uv.y*2-1,2);
 return vec2(cos(theta),1-2*uv.x*uv.x);
}
vec2 skyViewUv(vec3 direction,vec3 sun,float radius) {
 float horizon=skyHorizon(radius),theta=acos(clamp(direction.y,-1,1));
 float v=theta<horizon?.5*(1-sqrt(max(0,1-theta/horizon))):.5+.5*sqrt(clamp((theta-horizon)/(SKY_PI-horizon),0,1));
 float denom=length(direction.xz)*length(sun.xz);
 float cosAz=denom>1e-6?dot(direction.xz,sun.xz)/denom:1;
 return skySubUv(vec2(sqrt(clamp(.5-.5*cosAz,0,1)),v),vec2(192,108));
}
vec3 skyWeight(vec3 extinction,float distance) {
 vec3 tau=extinction*distance;
 return mix((1-exp(-tau))/max(extinction,vec3(1e-9)),distance*(1-tau*.5+tau*tau/6),lessThan(tau,vec3(1e-4)));
}
