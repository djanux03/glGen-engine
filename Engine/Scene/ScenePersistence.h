#pragma once
#include "json.hpp"
#include <charconv>
#include <string>

namespace scenePersistence {
// Older editor saves accumulated streamed terrain markers and collision-only
// proxies. They have no authored asset, script or hierarchy, and restoring them
// multiplies the streaming world's colliders every session. Restrict migration
// to these reserved names AND their generated structure, preserving edited nodes.
inline bool isLegacyTerrainProxy(const nlohmann::json &ent) {
  if (!ent.is_object() || !ent.contains("name") || !ent["name"].is_string()) return false;
  for (auto it = ent.begin(); it != ent.end(); ++it) {
    const auto &k = it.key();
    if (k != "id" && k != "name" && k != "transform" && k != "hierarchy" &&
        k != "lifecycle" && k != "mesh" && k != "rigidbody" && k != "collider") return false;
  }
  if (ent.contains("hierarchy")) {
    const auto &h = ent["hierarchy"];
    if (!h.is_object() || h.value("parent", 0u) != 0u ||
        (h.contains("children") && !h["children"].empty())) return false;
  }
  const auto name = ent["name"].get<std::string>();
  constexpr const char *prefix = "TerrainChunk_";
  if (name.rfind(prefix, 0) == 0) {
    const char *begin = name.data() + std::char_traits<char>::length(prefix), *end = name.data() + name.size();
    int x = 0, z = 0;
    const auto a = std::from_chars(begin, end, x);
    if (a.ec != std::errc{} || a.ptr == end || *a.ptr != '_') return false;
    const auto b = std::from_chars(a.ptr + 1, end, z);
    if (b.ec != std::errc{} || b.ptr != end) return false;
    if (!ent.contains("mesh")) return true;
    const auto &m = ent["mesh"];
    return m.is_object() && m.value("type", "None") == "None" &&
           m.value("assetId", "") == name;
  }
  if (ent.contains("mesh") || !ent.contains("rigidbody") || !ent.contains("collider")) return false;
  const auto &rb = ent["rigidbody"], &col = ent["collider"];
  if (!rb.is_object() || !col.is_object() || rb.value("type", "") != "Static") return false;
  return (name == "InteractiveTree" && col.value("shape", "") == "Capsule") ||
         (name == "CollidableRock" && col.value("shape", "") == "Sphere");
}
} // namespace scenePersistence
