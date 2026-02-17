#include "AssetManager.h"

#include "FBXModel.h"
#include "OBJModel.h"
#include "Shader.h"
#include "Texture.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>

namespace {
using json = nlohmann::json;

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}
} // namespace

AssetManager::~AssetManager() = default;

void AssetManager::setCookRoot(const std::string &cookRoot) {
  mCookRoot = cookRoot;
}

AssetType AssetManager::inferAssetType_(const std::string &path) {
  const std::string ext = toLower(std::filesystem::path(path).extension().string());
  if (ext == ".obj")
    return AssetType::OBJModel;
  if (ext == ".gltf" || ext == ".glb" || ext == ".fbx")
    return AssetType::GLTFModel;
  if (ext == ".hdr")
    return AssetType::HDRTexture;
  if (ext == ".vert" || ext == ".frag" || ext == ".glsl")
    return AssetType::ShaderProgram;
  return AssetType::Unknown;
}

std::string AssetManager::assetTypeToString_(AssetType t) {
  switch (t) {
  case AssetType::OBJModel:
    return "OBJModel";
  case AssetType::GLTFModel:
    return "GLTFModel";
  case AssetType::HDRTexture:
    return "HDRTexture";
  case AssetType::ShaderProgram:
    return "ShaderProgram";
  default:
    return "Unknown";
  }
}

std::filesystem::file_time_type
AssetManager::safeWriteTime_(const std::string &path) const {
  std::error_code ec;
  const auto t = std::filesystem::last_write_time(path, ec);
  if (ec)
    return {};
  return t;
}

std::string AssetManager::cookedPathFor_(const std::string &sourcePath) const {
  namespace fs = std::filesystem;
  fs::path src(sourcePath);
  fs::path cooked = fs::path(mCookRoot) / src.filename();
  return cooked.lexically_normal().string();
}

void AssetManager::writeImportMeta_(const ImportJob &job) const {
  json j;
  j["id"] = job.id;
  j["sourcePath"] = job.sourcePath;
  j["cookedPath"] = job.cookedPath;
  j["type"] = assetTypeToString_(job.type);
  j["status"] = job.status;
  j["warning"] = job.warning;
  j["dependencies"] = job.dependencies;

  const std::string metaPath = job.cookedPath + ".meta.json";
  std::ofstream out(metaPath);
  if (out.is_open())
    out << j.dump(2);
}

OBJHandle AssetManager::loadOBJ(const std::string &path) {
  const auto it = mOBJByPath.find(path);
  if (it != mOBJByPath.end()) {
    uint32_t idx = it->second;
    return OBJHandle{idx, mOBJ[idx].generation};
  }

  OBJRecord rec;
  rec.sourcePath = path;
  rec.dependencies = {path};
  rec.watchedTime = safeWriteTime_(path);
  rec.asset = std::make_unique<OBJModel>();
  const std::string cooked = cookedPathFor_(path);
  const std::string loadPath =
      std::filesystem::exists(cooked) ? cooked : path;
  if (!rec.asset->loadFromFile(loadPath)) {
    return {};
  }

  const uint32_t idx = (uint32_t)mOBJ.size();
  mOBJ.push_back(std::move(rec));
  mOBJByPath[path] = idx;
  return OBJHandle{idx, mOBJ[idx].generation};
}

GLTFHandle AssetManager::loadGLTF(const std::string &path) {
  const auto it = mGLTFByPath.find(path);
  if (it != mGLTFByPath.end()) {
    uint32_t idx = it->second;
    return GLTFHandle{idx, mGLTF[idx].generation};
  }

  GLTFRecord rec;
  rec.sourcePath = path;
  rec.dependencies = {path};
  rec.watchedTime = safeWriteTime_(path);
  rec.asset = std::make_unique<FBXModel>();
  const std::string cooked = cookedPathFor_(path);
  const std::string loadPath =
      std::filesystem::exists(cooked) ? cooked : path;
  if (!rec.asset->loadFromFile(loadPath)) {
    return {};
  }

  const uint32_t idx = (uint32_t)mGLTF.size();
  mGLTF.push_back(std::move(rec));
  mGLTFByPath[path] = idx;
  return GLTFHandle{idx, mGLTF[idx].generation};
}

OBJModel *AssetManager::getOBJ(OBJHandle h) {
  if (!h.valid() || h.index >= mOBJ.size())
    return nullptr;
  if (mOBJ[h.index].generation != h.generation)
    return nullptr;
  return mOBJ[h.index].asset.get();
}

FBXModel *AssetManager::getGLTF(GLTFHandle h) {
  if (!h.valid() || h.index >= mGLTF.size())
    return nullptr;
  if (mGLTF[h.index].generation != h.generation)
    return nullptr;
  return mGLTF[h.index].asset.get();
}

ShaderHandle AssetManager::registerShader(Shader *shader,
                                          const std::string &vertPath,
                                          const std::string &fragPath) {
  if (!shader)
    return {};

  ShaderRecord rec;
  rec.shader = shader;
  rec.vertPath = vertPath;
  rec.fragPath = fragPath;
  rec.dependencies = {vertPath, fragPath};
  rec.vertTime = safeWriteTime_(vertPath);
  rec.fragTime = safeWriteTime_(fragPath);

  const uint32_t idx = (uint32_t)mShaders.size();
  mShaders.push_back(std::move(rec));
  return ShaderHandle{idx, mShaders[idx].generation};
}

uint64_t AssetManager::queueImport(const std::string &path) {
  ImportJob job;
  job.id = mNextImportId++;
  job.sourcePath = path;
  job.type = inferAssetType_(path);
  job.status = "Queued";
  mImportJobs.push_back(job);
  return job.id;
}

void AssetManager::processImportQueue() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(mCookRoot, ec);

  for (auto &job : mImportJobs) {
    if (job.status == "Imported")
      continue;

    if (!fs::exists(job.sourcePath)) {
      job.status = "Failed";
      job.warning = "Source file does not exist";
      continue;
    }

    job.cookedPath = cookedPathFor_(job.sourcePath);
    job.dependencies = {job.sourcePath};

    std::error_code copyEc;
    fs::copy_file(job.sourcePath, job.cookedPath,
                  fs::copy_options::overwrite_existing, copyEc);
    if (copyEc) {
      job.status = "Failed";
      job.warning = copyEc.message();
      continue;
    }

    if (job.type == AssetType::Unknown) {
      job.status = "Imported";
      job.warning = "Unknown type: copied as passthrough";
    } else {
      job.status = "Imported";
      job.warning = "Passthrough cook (metadata + copy)";
    }

    writeImportMeta_(job);
  }
}

std::vector<std::string> AssetManager::pollHotReload() {
  std::vector<std::string> out;

  for (auto &rec : mOBJ) {
    auto t = safeWriteTime_(rec.sourcePath);
    if (t != std::filesystem::file_time_type{} && t != rec.watchedTime) {
      const std::string cooked = cookedPathFor_(rec.sourcePath);
      if (std::filesystem::exists(cooked)) {
        std::error_code ec;
        std::filesystem::copy_file(rec.sourcePath, cooked,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
      }
      const std::string loadPath =
          std::filesystem::exists(cooked) ? cooked : rec.sourcePath;
      if (rec.asset && rec.asset->loadFromFile(loadPath)) {
        rec.watchedTime = t;
        out.push_back("Reloaded OBJ: " + rec.sourcePath);
      }
    }
  }

  for (auto &rec : mGLTF) {
    auto t = safeWriteTime_(rec.sourcePath);
    if (t != std::filesystem::file_time_type{} && t != rec.watchedTime) {
      const std::string cooked = cookedPathFor_(rec.sourcePath);
      if (std::filesystem::exists(cooked)) {
        std::error_code ec;
        std::filesystem::copy_file(rec.sourcePath, cooked,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
      }
      const std::string loadPath =
          std::filesystem::exists(cooked) ? cooked : rec.sourcePath;
      if (rec.asset && rec.asset->loadFromFile(loadPath)) {
        rec.watchedTime = t;
        out.push_back("Reloaded GLTF/FBX: " + rec.sourcePath);
      }
    }
  }

  for (auto &rec : mShaders) {
    auto vt = safeWriteTime_(rec.vertPath);
    auto ft = safeWriteTime_(rec.fragPath);
    if ((vt != std::filesystem::file_time_type{} && vt != rec.vertTime) ||
        (ft != std::filesystem::file_time_type{} && ft != rec.fragTime)) {
      if (rec.shader && rec.shader->reload()) {
        rec.vertTime = vt;
        rec.fragTime = ft;
        out.push_back("Reloaded Shader: " + rec.vertPath + " + " + rec.fragPath);
      }
    }
  }

  return out;
}
