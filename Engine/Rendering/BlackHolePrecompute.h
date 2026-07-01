#pragma once

#include <glm/glm.hpp>

#include <vector>

struct BlackHolePrecomputeData {
  int rayDeflectionWidth = 512;
  int rayDeflectionHeight = 512;
  int rayInverseRadiusWidth = 64;
  int rayInverseRadiusHeight = 32;
  int blackBodyWidth = 256;
  int dopplerSize = 32;
  int skyCubeSize = 256;
  int noiseSize = 256;

  std::vector<float> rayDeflectionRG;
  std::vector<float> rayInverseRadiusRG;
  std::vector<float> blackBodyRGB;
  std::vector<float> dopplerRGB;
  std::vector<float> starCubeRGB;
  std::vector<float> galaxyCubeRGB;
  std::vector<unsigned char> noiseR;
  double generationMs = 0.0;
};

class BlackHolePrecompute {
public:
  static BlackHolePrecomputeData generate();
};
