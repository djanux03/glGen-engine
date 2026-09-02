#include "AssetManager.h"

#include "MeshParse.h"
#include "MeshPrimitives.h"
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

AssetManager::~AssetManager() {
  for (auto &rec : mOBJ) {
    if (rec.gpu && mBackend.destroyOBJ)
      mBackend.destroyOBJ(rec.gpu);
  }
  for (auto &rec : mGLTF) {
    if (rec.gpu && mBackend.destroyGLTF)
      mBackend.destroyGLTF(rec.gpu);
  }
  for (auto &rec : mUFBX) {
    if (rec.gpu && mBackend.destroyUFBX)
      mBackend.destroyUFBX(rec.gpu);
  }
}

void AssetManager::setGpuBackend(ModelGpuBackend backend) {
  mBackend = std::move(backend);
}

void AssetManager::setShaderReloader(std::function<bool(Shader *)> reloader) {
  mShaderReloader = std::move(reloader);
}

void AssetManager::setCookRoot(const std::string &cookRoot) {
  mCookRoot = cookRoot;
}

AssetType AssetManager::inferAssetType_(const std::string &path) {
  const std::string ext =
      toLower(std::filesystem::path(path).extension().string());
  if (ext == ".obj")
    return AssetType::OBJModel;
  if (ext == ".gltf" || ext == ".glb")
    return AssetType::GLTFModel;
  if (ext == ".fbx")
    return AssetType::UFBXModel;
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
  case AssetType::UFBXModel:
    return "UFBXModel";
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
  rec.cpu = makePrimitiveMesh(path);
  rec.runtimeAsset = false;

  if (!rec.cpu) {
    rec.dependencies = {path};
    rec.watchedTime = safeWriteTime_(path);
    const std::string cooked = cookedPathFor_(path);
    const std::string loadPath =
        std::filesystem::exists(cooked) ? cooked : path;
    rec.cpu = parseMeshOBJ(loadPath);
    if (!rec.cpu) {
      return {};
    }
  }

  if (mBackend.createOBJ) {
    rec.gpu = mBackend.createOBJ(*rec.cpu);
    if (!rec.gpu) {
      return {};
    }
  }

  const uint32_t idx = (uint32_t)mOBJ.size();
  // TODO: This always push_backs, creating a minor slot leak when loading previously released paths.
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
  const std::string cooked = cookedPathFor_(path);
  const std::string loadPath = std::filesystem::exists(cooked) ? cooked : path;
  rec.cpu = parseMeshGLTF(loadPath);
  if (!rec.cpu) {
    return {};
  }

  if (mBackend.createGLTF) {
    rec.gpu = mBackend.createGLTF(*rec.cpu);
    if (!rec.gpu) {
      return {};
    }
  }

  const uint32_t idx = (uint32_t)mGLTF.size();
  mGLTF.push_back(std::move(rec));
  mGLTFByPath[path] = idx;
  return GLTFHandle{idx, mGLTF[idx].generation};
}

UFBXHandle AssetManager::loadUFBX(const std::string &path) {
  const auto it = mUFBXByPath.find(path);
  if (it != mUFBXByPath.end()) {
    uint32_t idx = it->second;
    return UFBXHandle{idx, mUFBX[idx].generation};
  }

  UFBXRecord rec;
  rec.sourcePath = path;
  rec.dependencies = {path};
  rec.watchedTime = safeWriteTime_(path);
  const std::string cooked = cookedPathFor_(path);
  const std::string loadPath = std::filesystem::exists(cooked) ? cooked : path;
  rec.cpu = parseMeshFBX(loadPath);
  if (!rec.cpu) {
    return {};
  }

  if (mBackend.createUFBX) {
    rec.gpu = mBackend.createUFBX(*rec.cpu);
    if (!rec.gpu) {
      return {};
    }
  }

  const uint32_t idx = (uint32_t)mUFBX.size();
  mUFBX.push_back(std::move(rec));
  mUFBXByPath[path] = idx;
  return UFBXHandle{idx, mUFBX[idx].generation};
}

OBJHandle AssetManager::registerMeshData(const std::string &assetId,
                                         std::unique_ptr<MeshData> data) {
  if (assetId.empty() || !data)
    return {};
  if (data->sourcePath.empty())
    data->sourcePath = assetId;

  // Already registered: this is a replace, so reuse the slot (and therefore
  // keep outstanding handles valid) rather than minting a second record.
  const auto it = mOBJByPath.find(assetId);
  if (it != mOBJByPath.end() && it->second < mOBJ.size()) {
    auto &rec = mOBJ[it->second];
    rec.cpu = std::move(data);
    ++rec.contentVersion;
    if (rec.gpu && mBackend.reloadOBJ)
      mBackend.reloadOBJ(rec.gpu, *rec.cpu);
    return OBJHandle{it->second, rec.generation};
  }

  // Recycle a released slot rather than growing the vector unboundedly as
  // generated assets are registered and dropped. A slot is only free when it
  // holds neither GPU nor CPU data -- a synthetic asset carries CPU data with
  // no GPU model, so testing `gpu` alone (as the removed registerRuntimeOBJRaw
  // did) would hand out a live slot.
  for (uint32_t i = 0; i < (uint32_t)mOBJ.size(); ++i) {
    auto &slot = mOBJ[i];
    if (slot.gpu || slot.cpu || !slot.runtimeAsset)
      continue;
    slot.sourcePath = assetId;
    slot.dependencies.clear();
    slot.watchedTime = {};
    slot.cpu = std::move(data);
    ++slot.contentVersion;
    mOBJByPath[assetId] = i;
    return OBJHandle{i, slot.generation};
  }

  OBJRecord rec;
  rec.sourcePath = assetId;
  rec.cpu = std::move(data);
  rec.runtimeAsset = true;

  const uint32_t idx = (uint32_t)mOBJ.size();
  mOBJ.push_back(std::move(rec));
  mOBJByPath[assetId] = idx;
  return OBJHandle{idx, mOBJ[idx].generation};
}

bool AssetManager::replaceMeshData(const std::string &assetId,
                                   std::unique_ptr<MeshData> data) {
  if (!data)
    return false;
  const auto it = mOBJByPath.find(assetId);
  if (it == mOBJByPath.end() || it->second >= mOBJ.size())
    return false;
  return registerMeshData(assetId, std::move(data)).valid();
}

OBJHandle AssetManager::findMeshData(const std::string &assetId) const {
  const auto it = mOBJByPath.find(assetId);
  if (it == mOBJByPath.end() || it->second >= mOBJ.size())
    return {};
  return OBJHandle{it->second, mOBJ[it->second].generation};
}

uint32_t AssetManager::assetContentVersion(const std::string &assetId) const {
  const auto it = mOBJByPath.find(assetId);
  if (it == mOBJByPath.end() || it->second >= mOBJ.size())
    return 0;
  return mOBJ[it->second].contentVersion;
}

bool AssetManager::releaseOBJ(const std::string &assetId) {
  const auto it = mOBJByPath.find(assetId);
  if (it == mOBJByPath.end())
    return false;
  const uint32_t idx = it->second;
  mOBJByPath.erase(it);
  // Note: AssetLibrary::invalidate(assetId) needs to be called here to prevent stale cache entries,
  // but due to layering, AssetManager doesn't know about AssetLibrary.
  if (idx >= mOBJ.size())
    return false;

  auto &rec = mOBJ[idx];
  const bool wasRuntimeAsset = rec.runtimeAsset;
  if (rec.gpu && mBackend.destroyOBJ)
    mBackend.destroyOBJ(rec.gpu);
  rec.gpu = nullptr;
  rec.cpu.reset();
  rec.dependencies.clear();
  rec.watchedTime = {};
  rec.runtimeAsset = wasRuntimeAsset;
  ++rec.generation;
  return true;
}

AssetStats AssetManager::stats() const {
  AssetStats s{};

  for (const auto &rec : mOBJ) {
    if (!rec.gpu && !rec.cpu)
      continue;
    ++s.objLive;
    if (rec.runtimeAsset)
      ++s.objRuntimeLive;
  }

  for (const auto &rec : mGLTF)
    if (rec.gpu || rec.cpu)
      ++s.gltfLive;
  for (const auto &rec : mUFBX)
    if (rec.gpu || rec.cpu)
      ++s.ufbxLive;
  for (const auto &rec : mShaders)
    if (rec.shader)
      ++s.shaderLive;

  for (const auto &job : mImportJobs) {
    if (job.status == "Queued")
      ++s.importQueued;
    else if (job.status == "Imported")
      ++s.importImported;
    else if (job.status == "Failed")
      ++s.importFailed;
  }

  return s;
}

void *AssetManager::gpuOBJ_(OBJHandle h) {
  if (!h.valid() || h.index >= mOBJ.size())
    return nullptr;
  if (mOBJ[h.index].generation != h.generation)
    return nullptr;
  return mOBJ[h.index].gpu;
}

void *AssetManager::gpuGLTF_(GLTFHandle h) {
  if (!h.valid() || h.index >= mGLTF.size())
    return nullptr;
  if (mGLTF[h.index].generation != h.generation)
    return nullptr;
  return mGLTF[h.index].gpu;
}

void *AssetManager::gpuUFBX_(UFBXHandle h) {
  if (!h.valid() || h.index >= mUFBX.size())
    return nullptr;
  if (mUFBX[h.index].generation != h.generation)
    return nullptr;
  return mUFBX[h.index].gpu;
}

bool AssetManager::recenterOBJ(OBJHandle h, MeshData::Recenter mode) {
  if (!h.valid() || h.index >= mOBJ.size())
    return false;
  auto &rec = mOBJ[h.index];
  if (rec.generation != h.generation || !rec.cpu)
    return false;
  rec.cpu->recenter(mode);
  // These mutate the CPU mesh in place, so a render system that already
  // uploaded this asset is now holding stale geometry -- bump the content
  // version so it re-uploads. (Previously this only worked because every
  // caller happened to run before the first frame.)
  ++rec.contentVersion;
  if (rec.gpu && mBackend.reloadOBJ)
    return mBackend.reloadOBJ(rec.gpu, *rec.cpu);
  return true;
}

bool AssetManager::rotateOBJ(OBJHandle h, glm::vec3 degXYZ) {
  if (!h.valid() || h.index >= mOBJ.size())
    return false;
  auto &rec = mOBJ[h.index];
  if (rec.generation != h.generation || !rec.cpu)
    return false;
  rec.cpu->rotateEulerDeg(degXYZ);
  ++rec.contentVersion; // see recenterOBJ
  if (rec.gpu && mBackend.reloadOBJ)
    return mBackend.reloadOBJ(rec.gpu, *rec.cpu);
  return true;
}

const MeshData *AssetManager::getOBJData(OBJHandle h) const {
  if (!h.valid() || h.index >= mOBJ.size())
    return nullptr;
  if (mOBJ[h.index].generation != h.generation)
    return nullptr;
  return mOBJ[h.index].cpu.get();
}

const MeshData *AssetManager::getGLTFData(GLTFHandle h) const {
  if (!h.valid() || h.index >= mGLTF.size())
    return nullptr;
  if (mGLTF[h.index].generation != h.generation)
    return nullptr;
  return mGLTF[h.index].cpu.get();
}

const MeshData *AssetManager::getUFBXData(UFBXHandle h) const {
  if (!h.valid() || h.index >= mUFBX.size())
    return nullptr;
  if (mUFBX[h.index].generation != h.generation)
    return nullptr;
  return mUFBX[h.index].cpu.get();
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
    if (rec.runtimeAsset || !rec.cpu)
      continue;
    auto t = safeWriteTime_(rec.sourcePath);
    if (t != std::filesystem::file_time_type{} && t != rec.watchedTime) {
      const std::string cooked = cookedPathFor_(rec.sourcePath);
      if (std::filesystem::exists(cooked)) {
        std::error_code ec;
        std::filesystem::copy_file(
            rec.sourcePath, cooked,
            std::filesystem::copy_options::overwrite_existing, ec);
      }
      const std::string loadPath =
          std::filesystem::exists(cooked) ? cooked : rec.sourcePath;
      auto reparsed = parseMeshOBJ(loadPath);
      if (reparsed) {
        rec.cpu = std::move(reparsed);
        ++rec.contentVersion; // or render systems keep the pre-edit geometry
        bool gpuOk = true;
        if (rec.gpu && mBackend.reloadOBJ)
          gpuOk = mBackend.reloadOBJ(rec.gpu, *rec.cpu);
        if (gpuOk) {
          rec.watchedTime = t;
          out.push_back("Reloaded OBJ: " + rec.sourcePath);
        }
      }
    }
  }

  for (auto &rec : mGLTF) {
    if (!rec.cpu)
      continue;
    auto t = safeWriteTime_(rec.sourcePath);
    if (t != std::filesystem::file_time_type{} && t != rec.watchedTime) {
      const std::string cooked = cookedPathFor_(rec.sourcePath);
      if (std::filesystem::exists(cooked)) {
        std::error_code ec;
        std::filesystem::copy_file(
            rec.sourcePath, cooked,
            std::filesystem::copy_options::overwrite_existing, ec);
      }
      const std::string loadPath =
          std::filesystem::exists(cooked) ? cooked : rec.sourcePath;
      auto reparsed = parseMeshGLTF(loadPath);
      if (reparsed) {
        rec.cpu = std::move(reparsed);
        bool gpuOk = true;
        if (rec.gpu && mBackend.reloadGLTF)
          gpuOk = mBackend.reloadGLTF(rec.gpu, *rec.cpu);
        if (gpuOk) {
          rec.watchedTime = t;
          out.push_back("Reloaded GLTF/FBX: " + rec.sourcePath);
        }
      }
    }
  }

  for (auto &rec : mUFBX) {
    if (!rec.cpu)
      continue;
    auto t = safeWriteTime_(rec.sourcePath);
    if (t != std::filesystem::file_time_type{} && t != rec.watchedTime) {
      const std::string cooked = cookedPathFor_(rec.sourcePath);
      if (std::filesystem::exists(cooked)) {
        std::error_code ec;
        std::filesystem::copy_file(
            rec.sourcePath, cooked,
            std::filesystem::copy_options::overwrite_existing, ec);
      }
      const std::string loadPath =
          std::filesystem::exists(cooked) ? cooked : rec.sourcePath;
      auto reparsed = parseMeshFBX(loadPath);
      if (reparsed) {
        rec.cpu = std::move(reparsed);
        bool gpuOk = true;
        if (rec.gpu && mBackend.reloadGLTF)
          gpuOk = mBackend.reloadGLTF(rec.gpu, *rec.cpu);
        if (gpuOk) {
          rec.watchedTime = t;
          out.push_back("Reloaded GLTF/FBX: " + rec.sourcePath);
        }
      }
    }
  }

  for (auto &rec : mShaders) {
    auto vt = safeWriteTime_(rec.vertPath);
    auto ft = safeWriteTime_(rec.fragPath);
    if ((vt != std::filesystem::file_time_type{} && vt != rec.vertTime) ||
        (ft != std::filesystem::file_time_type{} && ft != rec.fragTime)) {
      if (rec.shader && mShaderReloader && mShaderReloader(rec.shader)) {
        rec.vertTime = vt;
        rec.fragTime = ft;
        out.push_back("Reloaded Shader: " + rec.vertPath + " + " +
                      rec.fragPath);
      }
    }
  }

  return out;
}
