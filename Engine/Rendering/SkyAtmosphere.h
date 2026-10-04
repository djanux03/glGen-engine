#pragma once
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

// Double-precision reference for the GPU LUT model. Kilometres avoid loss of
// precision in planet-radius squares; authoring/camera distances remain metres.
namespace atmosphere::sky {
constexpr double bottom=6371, top=6451, pi=3.141592653589793;
inline glm::dvec3 extinction(double height, double haze) {
  const double rayleigh=std::exp(-std::max(0.0,height)/8.5);
  const double mie=std::exp(-std::max(0.0,height)/1.2);
  const double ozone=std::max(0.0,1-std::abs(height-25)/15);
  return glm::dvec3(.005802,.013558,.0331)*rayleigh
      +glm::dvec3((.002+.022*haze*haze)*1.11*mie)
      +glm::dvec3(.00065,.001881,.000085)*ozone;
}
inline glm::dvec2 transmittanceParameters(glm::dvec2 uv) {
  const double H=std::sqrt((top-bottom)*(top+bottom)),rho=H*uv.y;
  const double r=std::sqrt(rho*rho+bottom*bottom);
  const double distance=(top-r)+uv.x*(rho+H-(top-r));
  return {r,distance>1e-12?std::clamp((H*H-rho*rho-distance*distance)/(2*r*distance),-1.0,1.0):1};
}
inline glm::dvec2 transmittanceUv(double r,double mu) {
  const double H=std::sqrt((top-bottom)*(top+bottom));
  const double rho=std::sqrt(std::max(0.0,(r-bottom)*(r+bottom)));
  const double distance=std::max(0.0,-r*mu+std::sqrt(std::max(0.0,r*r*(mu*mu-1)+top*top)));
  return {(distance-(top-r))/std::max(1e-9,rho+H-(top-r)),rho/H};
}
inline glm::dvec3 transmittance(double radius,double mu,double haze,int steps=256) {
  const double discriminant=radius*radius*(mu*mu-1)+bottom*bottom;
  if(mu<0&&discriminant>=0) return glm::dvec3(0); // planet blocks the emitter
  const double distance=std::max(0.0,-radius*mu+std::sqrt(std::max(0.0,radius*radius*(mu*mu-1)+top*top)));
  glm::dvec3 tau(0);double previous=0;
  for(int i=0;i<steps;++i) {
    const double u=double(i+1)/steps,next=distance*u*u,t=(previous+next)*.5;
    const double h=std::sqrt(radius*radius+t*t+2*radius*mu*t)-bottom;
    tau+=extinction(h,haze)*(next-previous);previous=next;
  }
  return glm::exp(-tau);
}
inline double ozoneDensity(double height) {return std::max(0.0,1-std::abs(height-25)/15);}
} // namespace atmosphere::sky
