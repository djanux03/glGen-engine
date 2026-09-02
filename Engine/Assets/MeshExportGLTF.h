#pragma once
#include "MeshData.h"
#include <string>

// Writes a self-contained static PBR GLB from backend-neutral MeshData.  This
// deliberately exports only the static contract supported by glGen v1; skins
// and animations are not silently fabricated for character assets.
bool exportMeshGLB(const MeshData &mesh, const std::string &path,
                   std::string &error);
