#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class OBJModel;
class FBXModel;
class UFBXModel;
class Shader;

enum class AssetType {
  Unknown = 0,
  OBJModel,
  GLTFModel,
  UFBXModel,
  HDRTexture,
  ShaderProgram,
};

template <typename Tag> struct AssetHandle {
  uint32_t index = 0xFFFFFFFFu;
  uint32_t generation = 0;
  bool valid() const { return index != 0xFFFFFFFFu; }
};

struct OBJAssetTag {};
struct GLTFAssetTag {};
struct UFBXAssetTag {};
struct HDRAssetTag {};
struct ShaderAssetTag {};

using OBJHandle = AssetHandle<OBJAssetTag>;
using GLTFHandle = AssetHandle<GLTFAssetTag>;
using UFBXHandle = AssetHandle<UFBXAssetTag>;
using HDRHandle = AssetHandle<HDRAssetTag>;
using ShaderHandle = AssetHandle<ShaderAssetTag>;

struct ImportJob {
  uint64_t id = 0;
  std::string sourcePath;
  AssetType type = AssetType::Unknown;
  std::string status;
  std::string warning;
  std::string cookedPath;
  std::vector<std::string> dependencies;
};

class AssetManager {
public:
  AssetManager() = default;
  ~AssetManager();

  void setCookRoot(const std::string &cookRoot);
  const std::string &cookRoot() const { return mCookRoot; }

  OBJHandle loadOBJ(const std::string &path);
  GLTFHandle loadGLTF(const std::string &path);
  UFBXHandle loadUFBX(const std::string &path);

  OBJModel *getOBJ(OBJHandle h);
  FBXModel *getGLTF(GLTFHandle h);
  UFBXModel *getUFBX(UFBXHandle h);

  ShaderHandle registerShader(Shader *shader, const std::string &vertPath,
                              const std::string &fragPath);

  uint64_t queueImport(const std::string &path);
  void processImportQueue();
  const std::vector<ImportJob> &importJobs() const { return mImportJobs; }

  // Returns human-readable reload messages
  std::vector<std::string> pollHotReload();

private:
  static AssetType inferAssetType_(const std::string &path);
  static std::string assetTypeToString_(AssetType t);

  struct OBJRecord {
    uint32_t generation = 1;
    std::string sourcePath;
    std::vector<std::string> dependencies;
    std::filesystem::file_time_type watchedTime{};
    std::unique_ptr<OBJModel> asset;
  };

  struct GLTFRecord {
    uint32_t generation = 1;
    std::string sourcePath;
    std::vector<std::string> dependencies;
    std::filesystem::file_time_type watchedTime{};
    std::unique_ptr<FBXModel> asset;
  };

  struct UFBXRecord {
    uint32_t generation = 1;
    std::string sourcePath;
    std::vector<std::string> dependencies;
    std::filesystem::file_time_type watchedTime{};
    std::unique_ptr<UFBXModel> asset;
  };

  struct ShaderRecord {
    uint32_t generation = 1;
    Shader *shader = nullptr; // non-owning, owned by runtime systems
    std::string vertPath;
    std::string fragPath;
    std::vector<std::string> dependencies;
    std::filesystem::file_time_type vertTime{};
    std::filesystem::file_time_type fragTime{};
  };

  std::filesystem::file_time_type safeWriteTime_(const std::string &path) const;
  std::string cookedPathFor_(const std::string &sourcePath) const;
  void writeImportMeta_(const ImportJob &job) const;

  std::string mCookRoot = "Build/cooked";
  uint64_t mNextImportId = 1;

  std::vector<OBJRecord> mOBJ;
  std::vector<GLTFRecord> mGLTF;
  std::vector<UFBXRecord> mUFBX;
  std::vector<ShaderRecord> mShaders;

  std::unordered_map<std::string, uint32_t> mOBJByPath;
  std::unordered_map<std::string, uint32_t> mGLTFByPath;
  std::unordered_map<std::string, uint32_t> mUFBXByPath;

  std::vector<ImportJob> mImportJobs;
};
