#pragma once
#include <glm/glm.hpp>
#include <array>
#include <algorithm>
#include <cmath>

namespace atmosphere::sky {
using Harmonics=std::array<glm::dvec3,9>;
inline std::array<double,9> harmonicBasis(glm::dvec3 d) {
  return {.28209479177387814,.4886025119029199*d.y,.4886025119029199*d.z,.4886025119029199*d.x,
    1.0925484305920792*d.x*d.y,1.0925484305920792*d.y*d.z,
    .31539156525252005*(3*d.z*d.z-1),1.0925484305920792*d.x*d.z,.5462742152960396*(d.x*d.x-d.y*d.y)};
}
inline glm::dvec3 diffuseIrradiance(const Harmonics& coefficients,glm::dvec3 normal) {
  const auto b=harmonicBasis(glm::normalize(normal));glm::dvec3 result(0);
  for(int i=0;i<9;++i)result+=coefficients[i]*b[i]*(i==0?1.0:i<4?2.0/3:.25);
  return glm::max(result,glm::dvec3(0)); // E/pi, ready for Lambert albedo
}
template<class Radiance> Harmonics projectIrradiance(Radiance&& radiance,int samples=4096) {
  Harmonics coefficients{};
  for(int i=0;i<samples;++i) {
    const double y=1-2*(i+.5)/samples,az=i*2.399963229728653;
    const glm::dvec3 direction(std::cos(az)*std::sqrt(1-y*y),y,std::sin(az)*std::sqrt(1-y*y));
    const auto b=harmonicBasis(direction);const auto L=radiance(direction);
    for(int j=0;j<9;++j)coefficients[j]+=L*b[j]*(12.566370614359172/samples);
  }
  return coefficients;
}
} // namespace atmosphere::sky
