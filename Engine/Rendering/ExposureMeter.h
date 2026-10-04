#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace atmosphere {
using LuminanceHistogram=std::array<uint32_t,256>;
// Bin zero is vacuum/black; positive bins cover -16..16 stops. The returned
// geometric mean trims percentile outliers, rather than letting a few sun or
// emissive pixels dominate the camera. Units are those of the input radiance.
inline double meteredLuminance(const LuminanceHistogram& histogram,double low=.05,double high=.95) {
  uint64_t count=0;for(int i=1;i<256;++i)count+=histogram[i];if(!count)return 0;
  const double begin=count*std::clamp(low,0.0,1.0),end=count*std::clamp(high,low,1.0);
  double position=0,weightedLog=0,weight=0;
  for(int i=1;i<256;++i) {
    const double next=position+histogram[i],included=std::max(0.0,std::min(next,end)-std::max(position,begin));
    weightedLog+=included*std::min(16.0,-16+(i-.5)*(32.0/254));weight+=included;position=next;
  }
  return weight>0?std::exp2(weightedLog/weight):0;
}
inline double exposureTarget(double luminance,double key,double minimum,double maximum) {
  return std::clamp(key/std::max(luminance,1e-12),minimum,maximum);
}
inline double adaptExposure(double current,double target,double alpha) {
  return std::exp2(std::log2(std::max(current,1e-12))*(1-alpha)+std::log2(std::max(target,1e-12))*alpha);
}
} // namespace atmosphere
