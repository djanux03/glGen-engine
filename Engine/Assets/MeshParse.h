#pragma once
#include "MeshData.h"
#include <memory>
#include <string>

// CPU-side model parsing — no graphics API involved. Returns null on failure.
std::unique_ptr<MeshData> parseMeshOBJ(const std::string &path);
std::unique_ptr<MeshData> parseMeshGLTF(const std::string &path);
std::unique_ptr<MeshData> parseMeshFBX(const std::string &path);

// Dispatches on extension (.obj / .gltf / .glb / .fbx).
std::unique_ptr<MeshData> parseMeshFile(const std::string &path);
