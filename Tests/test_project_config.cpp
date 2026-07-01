#include <doctest/doctest.h>
#include "ProjectConfig.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
// Write a temp JSON file and return its path.  Caller is responsible for
// cleanup (or let the OS handle it — these are tiny).
std::string writeTempJson(const std::string& content) {
  namespace fs = std::filesystem;
  auto p = fs::temp_directory_path() / "glgen_test_config.json";
  std::ofstream f(p);
  f << content;
  f.close();
  return p.string();
}
} // namespace

TEST_CASE("ProjectConfig — loadFromFile populates fields") {
  std::string path = writeTempJson(R"({
    "projectRoot": "/game",
    "shaderRoot": "shaders/glsl",
    "assetRoot": "art",
    "startupScene": "level1.json",
    "mainVertexShader": "my_vert.glsl",
    "mainFragmentShader": "my_frag.glsl"
  })");

  ProjectConfig cfg;
  CHECK(cfg.loadFromFile(path));
  CHECK(cfg.projectRoot == "/game");
  CHECK(cfg.shaderRoot == "shaders/glsl");
  CHECK(cfg.assetRoot == "art");
  CHECK(cfg.startupScene == "level1.json");
  CHECK(cfg.mainVertexShader == "my_vert.glsl");
  CHECK(cfg.mainFragmentShader == "my_frag.glsl");

  std::remove(path.c_str());
}

TEST_CASE("ProjectConfig — missing file returns false") {
  ProjectConfig cfg;
  CHECK_FALSE(cfg.loadFromFile("nonexistent_path_xyz.json"));
}

TEST_CASE("ProjectConfig — defaults survive partial JSON") {
  std::string path = writeTempJson(R"({ "projectRoot": "/partial" })");

  ProjectConfig cfg;
  CHECK(cfg.loadFromFile(path));
  CHECK(cfg.projectRoot == "/partial");
  // Everything else stays at default
  CHECK(cfg.shaderRoot == "shaders/glsl");
  CHECK(cfg.assetRoot == "assets");
  CHECK(cfg.mainVertexShader == "vertex_core.glsl");

  std::remove(path.c_str());
}

TEST_CASE("ProjectConfig — save then load roundtrip") {
  ProjectConfig original;
  original.projectRoot = "/roundtrip";
  original.startupScene = "test.json";
  original.mainVertexShader = "rt_vert.glsl";

  namespace fs = std::filesystem;
  auto path = (fs::temp_directory_path() / "glgen_rt_test.json").string();

  CHECK(original.saveToFile(path));

  ProjectConfig loaded;
  CHECK(loaded.loadFromFile(path));
  CHECK(loaded.projectRoot == original.projectRoot);
  CHECK(loaded.startupScene == original.startupScene);
  CHECK(loaded.mainVertexShader == original.mainVertexShader);

  std::remove(path.c_str());
}

TEST_CASE("ProjectConfig — shaderPath joins correctly") {
  ProjectConfig cfg;
  cfg.projectRoot = "/game";
  cfg.shaderRoot = "shaders/glsl";

  std::string result = cfg.shaderPath("vertex.glsl");
  // Should be /game/shaders/glsl/vertex.glsl (platform-normalized)
  CHECK(result.find("vertex.glsl") != std::string::npos);
  CHECK(result.find("game") != std::string::npos);
}

TEST_CASE("ProjectConfig — assetPath joins correctly") {
  ProjectConfig cfg;
  cfg.projectRoot = "/game";
  cfg.assetRoot = "assets";

  std::string result = cfg.assetPath("models/tree.fbx");
  CHECK(result.find("tree.fbx") != std::string::npos);
  CHECK(result.find("game") != std::string::npos);
}

TEST_CASE("ProjectConfig — blackHoleDefaults roundtrip") {
  ProjectConfig original;
  original.blackHoleDefaults.valid = true;
  original.blackHoleDefaults.worldMode = true;
  original.blackHoleDefaults.azimuth = 180.0f;
  original.blackHoleDefaults.exposure = 2.5f;
  original.blackHoleDefaults.quality = 3;

  namespace fs = std::filesystem;
  auto path = (fs::temp_directory_path() / "glgen_bh_test.json").string();

  CHECK(original.saveToFile(path));

  ProjectConfig loaded;
  CHECK(loaded.loadFromFile(path));
  CHECK(loaded.blackHoleDefaults.valid == true);
  CHECK(loaded.blackHoleDefaults.worldMode == true);
  CHECK(loaded.blackHoleDefaults.azimuth == doctest::Approx(180.0f));
  CHECK(loaded.blackHoleDefaults.exposure == doctest::Approx(2.5f));
  CHECK(loaded.blackHoleDefaults.quality == 3);

  std::remove(path.c_str());
}
