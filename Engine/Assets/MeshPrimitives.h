#pragma once
#include "MeshData.h"
#include <memory>
#include <string>
#include <vector>

// API-agnostic procedural primitive geometry (non-indexed triangle lists).
namespace PrimitiveGeometry {
std::vector<MeshVertex> cube();
std::vector<MeshVertex> sphere(int stacks = 24, int slices = 32);
std::vector<MeshVertex> plane();
std::vector<MeshVertex> cylinder(int segments = 32);
std::vector<MeshVertex> cone(int segments = 32);
} // namespace PrimitiveGeometry

// Builds MeshData for a "__primitive_<shape>" asset id (cube/sphere/plane/
// cylinder/cone). Returns null for any other id.
std::unique_ptr<MeshData> makePrimitiveMesh(const std::string &assetId);
