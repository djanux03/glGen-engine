#include <doctest/doctest.h>
#include "AtmosphereSettings.h"
#include "HeightOffsetGrid.h"
#include <array>

TEST_CASE("atmosphere transport has a finite vacuum and composes independently of steps") {
  using namespace atmosphere;
  for(double sigma : {0.,1e-12,1e-6,.01,1.}) {
    Medium m{sigma,glm::dvec3(sigma*.7)};
    const auto reference=segment(m,180);
    for(int steps : {1,4,64,96,512}) {
      Transport t;
      for(int i=0;i<steps;++i) t=append(t,segment(m,180./steps));
      CHECK(t.transmittance==doctest::Approx(reference.transmittance).epsilon(1e-10));
      CHECK(t.scattering.x==doctest::Approx(reference.scattering.x).epsilon(1e-10));
      CHECK(std::isfinite(t.scattering.x));
    }
  }
  CHECK(segment({0,glm::dvec3(2)},3).scattering.x==6);
}
TEST_CASE("height profile integral agrees with independent quadrature and reciprocity") {
  using namespace atmosphere;
  for(double y : {-15.,0.,4.,80.}) for(double dy : {-1.,-.2,0.,.2,1.}) {
    constexpr int samples=20000; constexpr double distance=120;
    double reference=0;
    for(int i=0;i<samples;++i)
      reference += .013*std::exp(-.07*std::max(0.,y+dy*(i+.5)*distance/samples))*distance/samples;
    const double integral=heightIntegral(y,dy,distance,.013,.07,0);
    CHECK(integral==doctest::Approx(reference).epsilon(1e-5));
    CHECK(integral==doctest::Approx(heightIntegral(y+dy*distance,-dy,distance,.013,.07,0)).epsilon(1e-10));
  }
  CHECK(std::isfinite(heightIntegral(10000,-1,1000,.1,10,0)));
  CHECK(heightIntegral(10000,-1,1000,.1,10,0)==0);
}
TEST_CASE("HG is normalized and forward direction points at the emitter") {
  using namespace atmosphere;
  for(double g : {-.95,-.5,0.,.55,.87,.95}) {
    double integral=0;
    constexpr int samples=200000;
    for(int i=0;i<samples;++i) integral+=hg(-1.+2.*(i+.5)/samples,g)*12.566370614359172/samples;
    CHECK(integral==doctest::Approx(1).epsilon(.005));
  }
  CHECK(hg(1,.87)>hg(-1,.87));
}
TEST_CASE("atmosphere version 2 settings sanitize and round trip") {
  using namespace atmosphere;
  Settings s; s.quality=Quality::High; s.range=260; s.dustAlbedo={.8,.7,.4};
  Settings restored; decode(encode(s),restored);
  CHECK(encode(s)==encode(restored));
  decode({{"historyWeight",100},{"groundExtinction",-1},{"dustAlbedo",{2,-1,.5}}},restored);
  CHECK(restored.historyWeight==.9f); CHECK(restored.groundExtinction==0);
  CHECK(restored.dustAlbedo==glm::vec3(1,0,.5));
  for(int n : {64,96}) {
    CHECK(sliceBoundary(0,n,180)==0); CHECK(sliceBoundary(1,n,180)==.05);
    CHECK(sliceBoundary(n,n,180)==doctest::Approx(180));
    for(int i=0;i<n;++i) CHECK(sliceBoundary(i+1,n,180)>sliceBoundary(i,n,180));
  }
}
TEST_CASE("legacy atmosphere aliases migrate with explicit nested precedence") {
  struct Params {
    atmosphere::Settings atmosphere;
    float fogDensity=.012f,fogHeightFalloff=.05f,fogHeightRef=-3,fogStart=20,fogAnisotropy=.4f;
    bool volumetricEnabled=true;
    float volumetricDensityScale=2,volumetricAnisotropy=.8f,volumetricTintStrength=.5f,volumetricIntensity=3;
    float bloomIntensity=.04f;
    glm::vec3 volumetricTintColor{1,.5f,.2f};
  } p;
  atmosphere::read(nlohmann::json::object(),p);
  CHECK(p.atmosphere.groundExtinction==.012f);
  CHECK(p.atmosphere.dustExtinction==.002f);
  CHECK(p.atmosphere.dustAlbedo==glm::vec3(1));
  atmosphere::read({{"fogDensity",.012},{"atmosphere",{{"groundExtinction",.007},{"dustAlbedo",{.7,.6,.3}}}}},p);
  CHECK(p.fogDensity==.007f);
  const auto saved=atmosphere::encode(atmosphere::fromLegacy(p));
  atmosphere::read({{"atmosphere",saved}},p);
  CHECK(atmosphere::encode(atmosphere::fromLegacy(p))==saved);
}
TEST_CASE("fog field height snapshots remain immutable across edits and regeneration") {
  HeightOffsetGrid edits;
  edits.applyBrush({5,5},4,2,10,11);
  const auto snapshot=edits.snapshot();
  const float original=snapshot->sampleBilinear({5,5},10,11);
  CHECK(original>0);
  edits.applyBrush({5,5},4,8,10,11);
  CHECK(snapshot->sampleBilinear({5,5},10,11)==original);
  edits.clear();
  CHECK(snapshot->sampleBilinear({5,5},10,11)==original);
  CHECK(edits.sampleBilinear({5,5},10,11)==0);
}
TEST_CASE("fog light participation and shadows survive the shared authoring codec") {
  struct Light {
    glm::vec3 position{0},color{1};
    float radius=10,intensity=1,volumetricParticipation=1;
    bool volumetricShadows=true;
  };
  struct Params { std::array<Light,4> pointLights; unsigned pointLightCount=0; } p,restored;
  atmosphere::readLights({{"pointLights",{{{"position",{3,4,5}},
    {"radius",20},{"intensity",80},{"volumetricParticipation",.4},
    {"volumetricShadows",false}}}}},p);
  const auto encoded=atmosphere::encodeLights(p);
  atmosphere::readLights({{"pointLights",encoded}},restored);
  CHECK(atmosphere::encodeLights(restored)==encoded);
  CHECK(restored.pointLights[0].volumetricParticipation==.4f);
  CHECK_FALSE(restored.pointLights[0].volumetricShadows);
  atmosphere::readLights({{"pointLights",{{{"radius",-4},{"intensity",-2},
    {"volumetricParticipation",5}}}}},p);
  CHECK(p.pointLights[0].radius==.1f);
  CHECK(p.pointLights[0].intensity==0);
  CHECK(p.pointLights[0].volumetricParticipation==1);
  atmosphere::Settings settings;
  atmosphere::decode({{"quality","high"}},settings);
  CHECK(settings.quality==atmosphere::Quality::High);
  atmosphere::decode({{"cloudHistory",false},{"cloudShadows",false},{"physicalSky",false}},settings);
  const auto authored=atmosphere::encode(settings);
  atmosphere::Settings loaded;atmosphere::decode(authored,loaded);
  CHECK_FALSE(loaded.cloudHistory);CHECK_FALSE(loaded.cloudShadows);CHECK_FALSE(loaded.physicalSky);
  atmosphere::decode({{"histogramExposure",false},{"exposureKey",.3},{"exposureMin",.02},{"exposureMax",20},
                      {"valleyPooling",false},{"valleyExtinction",.004},{"valleyDepth",12}},settings);
  atmosphere::decode(atmosphere::encode(settings),loaded);
  CHECK_FALSE(loaded.histogramExposure);CHECK(loaded.exposureKey==.3f);
  CHECK(loaded.exposureMin==.02f);CHECK(loaded.exposureMax==20);
  CHECK_FALSE(loaded.valleyPooling);CHECK(loaded.valleyExtinction==.004f);CHECK(loaded.valleyDepth==12);
  atmosphere::decode({{"exposureMin",30},{"exposureMax",1}},loaded);
  CHECK(loaded.exposureMax==loaded.exposureMin);
}

#include "Rendering/SkyAtmosphere.h"
#include "Rendering/SkyIrradiance.h"
#include "Terrain/FogPooling.h"
#include "Rendering/ExposureMeter.h"
TEST_CASE("histogram exposure trims outliers and adapts in stops") {
  atmosphere::LuminanceHistogram uniform{};uniform[128]=900;
  auto outliers=uniform;outliers[1]=50;outliers[255]=50;outliers[0]=10000;
  const double middle=atmosphere::meteredLuminance(uniform);
  CHECK(middle>1);CHECK(middle<1.05);
  CHECK(atmosphere::meteredLuminance(outliers)==doctest::Approx(middle));
  CHECK(atmosphere::meteredLuminance({})==0);
  CHECK(atmosphere::exposureTarget(1,.18,.01,64)==doctest::Approx(.18));
  CHECK(atmosphere::exposureTarget(0,.18,.01,64)==64);
  const double half=atmosphere::adaptExposure(1,16,.5);CHECK(half==doctest::Approx(4));
  CHECK(atmosphere::adaptExposure(half,16,.5)==doctest::Approx(atmosphere::adaptExposure(1,16,.75)));
}
TEST_CASE("cold-air basin potential respects spill height and open drainage") {
  std::vector<float> heights(49,10);
  for(int z=1;z<6;++z)for(int x=1;x<6;++x)heights[z*7+x]=2;
  heights[3*7+3]=-3;
  const auto closed=atmosphere::basinDepths(heights,7,7);
  CHECK(closed[3*7+3]==13);CHECK(closed[2*7+2]==8);CHECK(closed[0]==0);
  CHECK(atmosphere::basinDepths(heights,7,7)==closed);
  // A level outlet drains the bowl, but the smaller central depression still
  // pools to the 2 m spill level. Diagonal outlets count on the 8-connected DEM.
  heights[0*7+2]=2;
  const auto drained=atmosphere::basinDepths(heights,7,7);
  CHECK(drained[2*7+2]==0);CHECK(drained[3*7+3]==5);
  heights[2*7+2]=std::numeric_limits<float>::quiet_NaN();
  const auto missing=atmosphere::basinDepths(heights,7,7);CHECK(missing[2*7+2]==0);
  CHECK(atmosphere::basinDepths(heights,6,7).empty());
}
TEST_CASE("sky harmonics conserve uniform radiance and orient linear diffuse light") {
  using namespace atmosphere::sky;
  const glm::dvec3 uniform(.7,.5,.2),gradient(.12,.09,.03);
  const auto constant=projectIrradiance([&](glm::dvec3){return uniform;});
  const auto linear=projectIrradiance([&](glm::dvec3 d){return uniform+gradient*d.y;});
  for(glm::dvec3 N:{glm::dvec3(1,0,0),{0,1,0},{0,-1,0},{0,0,1},glm::normalize(glm::dvec3(1,2,3))}) {
    const auto C=diffuseIrradiance(constant,N),L=diffuseIrradiance(linear,N);
    for(int c=0;c<3;++c) {
      CHECK(C[c]==doctest::Approx(uniform[c]).epsilon(.0002));
      CHECK(L[c]==doctest::Approx(uniform[c]+gradient[c]*N.y*2/3).epsilon(.0002));
    }
  }
  CHECK(diffuseIrradiance(linear,{0,1,0}).r>diffuseIrradiance(linear,{0,-1,0}).r);
}
TEST_CASE("physical sky ozone layer and transmittance LUT mapping") {
  using namespace atmosphere::sky;
  CHECK(ozoneDensity(0)==0);CHECK(ozoneDensity(10)==0);
  CHECK(ozoneDensity(25)==1);CHECK(ozoneDensity(40)==0);
  for(double y:{0.0,.1,.5,.9,.999})for(double x:{0.0,.1,.5,.9,1.0}) {
    const glm::dvec2 uv(x,y),parameters=transmittanceParameters(uv);
    const glm::dvec2 restored=transmittanceUv(parameters.x,parameters.y);
    CHECK(restored.x==doctest::Approx(x).epsilon(1e-6));
    CHECK(restored.y==doctest::Approx(y).epsilon(1e-6));
  }
  for(double haze:{0.0,.4,1.0})for(double mu:{0.0,.03,.3,1.0}) {
    const auto reference=transmittance(bottom+.002,mu,haze,8192);
    const auto fast=transmittance(bottom+.002,mu,haze,256);
    for(int c=0;c<3;++c) {
      CHECK(fast[c]>=0);CHECK(fast[c]<=1);
      CHECK(std::abs(fast[c]-reference[c])<.002);
    }
  }
  CHECK(transmittance(bottom+.002,-.1,.4)==glm::dvec3(0));
  CHECK(transmittance(bottom+.002,1,.4).r>transmittance(bottom+.002,1,.4).b);
}
