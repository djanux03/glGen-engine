#pragma once

#include "MeshData.h"

#include <cstdint>
#include <filesystem>
#include <functional>
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

// GPU upload half of the asset pipeline. The active rendering backend
// registers these so AssetManager itself stays graphics-API-free. Without a
// backend, assets still parse to CPU MeshData (handles stay valid; get*()
// model pointers are null).
struct ModelGpuBackend {
  std::function<void *(const MeshData &)> createOBJ;
  std::function<void *(const MeshData &)> createGLTF;
  std::function<void *(const MeshData &)> createUFBX;
  // In-place reloads (hot reload) so raw model pointers stay valid.
  std::function<bool(void *, const MeshData &)> reloadOBJ;
  std::function<bool(void *, const MeshData &)> reloadGLTF;
  std::function<void(void *)> destroyOBJ;
  std::function<void(void *)> destroyGLTF;
  std::function<void(void *)> destroyUFBX;
};

struct ImportJob {
  uint64_t id = 0;
  std::string sourcePath;
  AssetType type = AssetType::Unknown;
  std::string status;
  std::string warning;
  std::string cookedPath;
  std::vector<std::string> dependencies;
};

struct AssetStats {
  uint32_t objLive = 0;
  uint32_t objRuntimeLive = 0;
  uint32_t gltfLive = 0;
  uint32_t ufbxLive = 0;
  uint32_t shaderLive = 0;
  uint32_t importQueued = 0;
  uint32_t importImported = 0;
  uint32_t importFailed = 0;
};

class AssetManager {
public:
  AssetManager() = default;
  ~AssetManager();

  // Must be called before any load*() when a rendering backend is active.
  void setGpuBackend(ModelGpuBackend backend);
  void setShaderReloader(std::function<bool(Shader *)> reloader);

  void setCookRoot(const std::string &cookRoot);
  const std::string &cookRoot() const { return mCookRoot; }

  OBJHandle loadOBJ(const std::string &path);
  GLTFHandle loadGLTF(const std::string &path);
  UFBXHandle loadUFBX(const std::string &path);

  // ---- Synthetic (in-memory) assets --------------------------------------
  // Registers a caller-built CPU mesh under an id that is NOT a file path --
  // procedurally generated content, by convention "gen://<generator>/<hash>".
  // Ownership transfers here; the MeshData is KEPT (unlike the removed
  // registerRuntimeOBJRaw, which stored only a GPU model and dropped the CPU
  // data -- VulkanRenderSystem resolves meshes through getOBJData(), so a
  // record without it is invisible to the renderer).
  //
  // Registering an id that already exists replaces it, exactly as
  // replaceMeshData() would. Returns an invalid handle if `assetId` is empty
  // or `data` is null.
  //
  // Registering over an id that was loaded from a FILE leaves that file's
  // watch intact, so the next pollHotReload() re-parses the file and discards
  // the supplied mesh. Use a synthetic id unless that is what you want.
  OBJHandle registerMeshData(const std::string &assetId,
                             std::unique_ptr<MeshData> data);

  // Swaps in new geometry for an already-registered asset and bumps its
  // content version, WITHOUT invalidating outstanding OBJHandles (see
  // contentVersion's note below on why that distinction matters). Render
  // systems poll assetContentVersion() to notice and re-upload. Returns false
  // if `assetId` was never registered.
  bool replaceMeshData(const std::string &assetId,
                       std::unique_ptr<MeshData> data);

  // Lookup-only counterpart to loadOBJ() for synthetic ids: never touches the
  // filesystem or the cook cache. Returns an invalid handle if unregistered.
  OBJHandle findMeshData(const std::string &assetId) const;

  // Monotonic counter bumped every time an asset's CPU geometry changes
  // (replaceMeshData, recenterOBJ, rotateOBJ, an OBJ hot reload). 0 = unknown
  // asset; live assets start at 1. This is deliberately NOT
  // AssetHandle::generation: that tracks handle IDENTITY (slot reuse, which
  // must invalidate stale handles), while this tracks CONTENT (same asset,
  // new bytes -- handles stay valid).
  //
  // OBJ/synthetic records only. glTF and FBX assets always report 0 ("never
  // stale"); they have no equivalent counter yet, so a render system caching
  // them will not notice a hot reload. Generated content is all
  // OBJ-record-backed, so this is a gap for imported models, not for the
  // generator pipeline.
  uint32_t assetContentVersion(const std::string &assetId) const;

  bool releaseOBJ(const std::string &assetId);

  // GPU model access. Null when no GPU backend is installed (the CPU MeshData
  // is still available via get*Data()).
  OBJModel *getOBJ(OBJHandle h) {
    return reinterpret_cast<OBJModel *>(gpuOBJ_(h));
  }
  FBXModel *getGLTF(GLTFHandle h) {
    return reinterpret_cast<FBXModel *>(gpuGLTF_(h));
  }
  UFBXModel *getUFBX(UFBXHandle h) {
    return reinterpret_cast<UFBXModel *>(gpuUFBX_(h));
  }

  // CPU-side parsed data (bounds, geometry, materials). Null for runtime
  // (procedurally registered) assets.
  const MeshData *getOBJData(OBJHandle h) const;
  const MeshData *getGLTFData(GLTFHandle h) const;
  const MeshData *getUFBXData(UFBXHandle h) const;

  // Recenters an off-origin authored model (CPU data + GPU reload when a
  // backend is installed). Idempotent; affects every instance of the asset.
  bool recenterOBJ(OBJHandle h, MeshData::Recenter mode);

  // Bakes a fixed corrective Euler rotation (degrees) into an off-axis
  // authored model (CPU data + GPU reload when a backend is installed) --
  // e.g. a tree model whose trunk runs along local X instead of Y. NOT
  // idempotent (unlike recenterOBJ); call once, before recenterOBJ.
  bool rotateOBJ(OBJHandle h, glm::vec3 degXYZ);

  ShaderHandle registerShader(Shader *shader, const std::string &vertPath,
                              const std::string &fragPath);

  uint64_t queueImport(const std::string &path);
  void processImportQueue();
  const std::vector<ImportJob> &importJobs() const { return mImportJobs; }
  AssetStats stats() const;

  // Returns human-readable reload messages
  std::vector<std::string> pollHotReload();

private:
  static AssetType inferAssetType_(const std::string &path);
  static std::string assetTypeToString_(AssetType t);

  struct OBJRecord {
    uint32_t generation = 1;
    // See assetContentVersion(): bumped on in-place CPU geometry changes,
    // independent of `generation`.
    uint32_t contentVersion = 1;
    std::string sourcePath;
    std::vector<std::string> dependencies;
    std::filesystem::file_time_type watchedTime{};
    std::unique_ptr<MeshData> cpu;
    void *gpu = nullptr; // OBJModel*, owned; destroyed via backend
    bool runtimeAsset = false;
  };

  struct GLTFRecord {
    uint32_t generation = 1;
    std::string sourcePath;
    std::vector<std::string> dependencies;
    std::filesystem::file_time_type watchedTime{};
    std::unique_ptr<MeshData> cpu;
    void *gpu = nullptr; // FBXModel*, owned; destroyed via backend
  };

  struct UFBXRecord {
    uint32_t generation = 1;
    std::string sourcePath;
    std::vector<std::string> dependencies;
    std::filesystem::file_time_type watchedTime{};
    std::unique_ptr<MeshData> cpu;
    void *gpu = nullptr; // UFBXModel*, owned; destroyed via backend
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

  void *gpuOBJ_(OBJHandle h);
  void *gpuGLTF_(GLTFHandle h);
  void *gpuUFBX_(UFBXHandle h);

  std::filesystem::file_time_type safeWriteTime_(const std::string &path) const;
  std::string cookedPathFor_(const std::string &sourcePath) const;
  void writeImportMeta_(const ImportJob &job) const;

  std::string mCookRoot = "Build/cooked";
  uint64_t mNextImportId = 1;

  ModelGpuBackend mBackend;
  std::function<bool(Shader *)> mShaderReloader;

  std::vector<OBJRecord> mOBJ;
  std::vector<GLTFRecord> mGLTF;
  std::vector<UFBXRecord> mUFBX;
  std::vector<ShaderRecord> mShaders;

  std::unordered_map<std::string, uint32_t> mOBJByPath;
  std::unordered_map<std::string, uint32_t> mGLTFByPath;
  std::unordered_map<std::string, uint32_t> mUFBXByPath;

  std::vector<ImportJob> mImportJobs;
};
