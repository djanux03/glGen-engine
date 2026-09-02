// glgen-bake — offline mesh conditioning, from a source file to a scatter-ready OBJ.
//
// GenImport.cpp says it plainly: "The fetch/conditioning half deliberately
// lives OUTSIDE the engine (a separate offline CLI)." This is that CLI for the
// case glgen-fetch does not cover -- when the result has to land on disk as a
// plain mesh rather than as a recipe.
//
// The distinction matters for scatter layers specifically. A recipe is
// regenerated at every startup; ScatterLayer::meshPath is read straight off
// disk. Pointing a scatter layer at a raw 2-million-triangle download means
// paying the parse AND the quadric decimation on every single launch, so the
// conditioning has to be baked once and committed.
//
// What it does, in order (the same order AssetManager and GenImport use, for
// the reason documented there -- rotate before recenter, or a model corrected
// to upright ends up pivoted around what used to be its side):
//   parse -> rotate -> normalize height -> decimate -> recenter -> write
//
// --split separates disconnected pieces into their own files first. Downloaded
// "tree" assets are routinely a clump of several trunks sharing one ground
// disc, which is one mesh as far as the file is concerned and three separate
// props as far as a scatter layer should be concerned.
//
//   glgen-bake --in assets/custom_trees/trees.obj --out assets/x/tree.obj \
//              --normalize-height 9 --max-tris 4000 --split --min-component 0.02

#include "Assets/MeshData.h"
#include "Assets/MeshParse.h"
#include "Generators/MeshDecimate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Options {
  std::string in;
  std::string out;
  size_t maxTris = 0;
  float normalizeHeight = 0.0f;
  glm::vec3 rotateDeg{0.0f};
  std::string recenter = "baseY";
  bool split = false;
  // Components smaller than this fraction of the largest one are dropped:
  // scanned assets carry stray shards, and a shard promoted to its own scatter
  // prop is worse than no prop.
  float minComponent = 0.02f;
  std::string material;
  // With --split, normalize each PIECE to --normalize-height rather than the
  // bundle. An asset pack laid out as one file is a set of props that happen
  // to share a scene, not one object: normalizing the bundle leaves every
  // piece at whatever fraction of the layout it occupied, which for a
  // 16-model tree pack meant 1-5 m "12 metre" trees.
  bool normalizeEach = false;
  // Only these component indices are written (after the size sort, so 0 is
  // the largest). Empty = all of them.
  std::vector<size_t> keep;
  // Parallel to `keep`: the filename stem to use for each kept piece, so the
  // output is conifer.obj / broadleaf.obj rather than tree_2.obj.
  std::vector<std::string> names;
  std::string albedoTex, normalTex, roughnessTex;
};

[[noreturn]] void usage(int code) {
  std::fprintf(stderr,
               "glgen-bake --in <mesh> --out <out.obj> [options]\n"
               "  --max-tris N          decimate to at most N triangles "
               "(per output file when splitting)\n"
               "  --normalize-height M  rescale so the Y extent is M metres\n"
               "  --rotate x,y,z        corrective rotation in degrees, baked in\n"
               "  --recenter MODE       none | baseY (default) | center\n"
               "  --split               write each connected piece separately\n"
               "  --min-component F     drop pieces under F of the largest (default 0.02)\n"
               "  --normalize-each      with --split, normalize each piece, not the bundle\n"
               "  --keep i,j,k          write only these component indices (largest first)\n"
               "  --names a,b,c         filename stems for the kept components\n"
               "  --material NAME       mtllib/usemtl name to reference\n"
               "  --albedo FILE         write an .mtl with this map_Kd\n"
               "  --normal FILE         ... and this normal map\n"
               "  --roughness FILE      ... and this map_Pr\n");
  std::exit(code);
}

std::vector<std::string> splitCsv(const std::string &s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t comma = s.find(',', start);
    const size_t end = (comma == std::string::npos) ? s.size() : comma;
    if (end > start)
      out.push_back(s.substr(start, end - start));
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return out;
}

bool parseArgs(int argc, char **argv, Options &o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char *what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "glgen-bake: %s needs a value\n", what);
        usage(2);
      }
      return argv[++i];
    };
    if (a == "--in")
      o.in = next("--in");
    else if (a == "--out")
      o.out = next("--out");
    else if (a == "--max-tris")
      o.maxTris = static_cast<size_t>(std::stoul(next("--max-tris")));
    else if (a == "--normalize-height")
      o.normalizeHeight = std::stof(next("--normalize-height"));
    else if (a == "--recenter")
      o.recenter = next("--recenter");
    else if (a == "--split")
      o.split = true;
    else if (a == "--min-component")
      o.minComponent = std::stof(next("--min-component"));
    else if (a == "--normalize-each")
      o.normalizeEach = true;
    else if (a == "--keep") {
      for (const std::string &v : splitCsv(next("--keep")))
        o.keep.push_back(static_cast<size_t>(std::stoul(v)));
    } else if (a == "--names")
      o.names = splitCsv(next("--names"));
    else if (a == "--material")
      o.material = next("--material");
    else if (a == "--albedo")
      o.albedoTex = next("--albedo");
    else if (a == "--normal")
      o.normalTex = next("--normal");
    else if (a == "--roughness")
      o.roughnessTex = next("--roughness");
    else if (a == "--rotate") {
      const std::string v = next("--rotate");
      if (std::sscanf(v.c_str(), "%f,%f,%f", &o.rotateDeg.x, &o.rotateDeg.y,
                      &o.rotateDeg.z) != 3) {
        std::fprintf(stderr, "glgen-bake: --rotate wants x,y,z\n");
        return false;
      }
    } else if (a == "-h" || a == "--help")
      usage(0);
    else {
      std::fprintf(stderr, "glgen-bake: unknown argument '%s'\n", a.c_str());
      return false;
    }
  }
  return !o.in.empty() && !o.out.empty();
}

size_t triangleCount(const MeshData &mesh) {
  size_t n = 0;
  for (const MeshSubmeshData &sm : mesh.submeshes)
    n += (sm.indices.empty() ? sm.vertices.size() : sm.indices.size()) / 3;
  return n;
}

// Union-find over welded positions. Vertices are split at UV seams in every
// exported format, so connectivity has to be judged by position, not by index
// -- otherwise a single tree comes back as hundreds of "components".
struct DisjointSet {
  std::vector<uint32_t> parent;
  explicit DisjointSet(size_t n) : parent(n) {
    std::iota(parent.begin(), parent.end(), 0u);
  }
  uint32_t find(uint32_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }
  void unite(uint32_t a, uint32_t b) {
    a = find(a);
    b = find(b);
    if (a != b)
      parent[b] = a;
  }
};

struct PosKey {
  int32_t x, y, z;
  bool operator==(const PosKey &o) const {
    return x == o.x && y == o.y && z == o.z;
  }
};
struct PosKeyHash {
  size_t operator()(const PosKey &k) const {
    size_t h = 1469598103934665603ull;
    for (int32_t v : {k.x, k.y, k.z}) {
      h ^= static_cast<size_t>(static_cast<uint32_t>(v));
      h *= 1099511628211ull;
    }
    return h;
  }
};

// Splits one submesh into connected components, each returned as a whole
// MeshData so the rest of the pipeline (decimate, recenter, write) does not
// need a separate code path for the split case.
std::vector<MeshData> splitComponents(const MeshData &mesh, float minFraction) {
  std::vector<MeshData> out;
  for (const MeshSubmeshData &sm : mesh.submeshes) {
    const size_t vcount = sm.vertices.size();
    if (vcount == 0)
      continue;
    std::vector<uint32_t> idx = sm.indices;
    if (idx.empty()) {
      idx.resize(vcount);
      std::iota(idx.begin(), idx.end(), 0u);
    }

    // Weld: quantize to 1e-5 of the mesh extent so float noise across a seam
    // does not read as a gap.
    glm::vec3 mn, mx;
    mesh.getGlobalBounds(mn, mx);
    const float scale = std::max({mx.x - mn.x, mx.y - mn.y, mx.z - mn.z, 1e-6f});
    const float quant = scale * 1e-5f;
    std::unordered_map<PosKey, uint32_t, PosKeyHash> weld;
    weld.reserve(vcount * 2);
    std::vector<uint32_t> welded(vcount);
    for (size_t v = 0; v < vcount; ++v) {
      const glm::vec3 &p = sm.vertices[v].pos;
      PosKey k{static_cast<int32_t>(std::lround(p.x / quant)),
               static_cast<int32_t>(std::lround(p.y / quant)),
               static_cast<int32_t>(std::lround(p.z / quant))};
      auto it = weld.emplace(k, static_cast<uint32_t>(v));
      welded[v] = it.first->second;
    }

    DisjointSet ds(vcount);
    for (size_t v = 0; v < vcount; ++v)
      ds.unite(static_cast<uint32_t>(v), welded[v]);
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
      ds.unite(idx[t], idx[t + 1]);
      ds.unite(idx[t], idx[t + 2]);
    }

    // Bucket triangles by their root.
    std::unordered_map<uint32_t, std::vector<uint32_t>> byRoot;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
      std::vector<uint32_t> &tri = byRoot[ds.find(idx[t])];
      tri.push_back(idx[t]);
      tri.push_back(idx[t + 1]);
      tri.push_back(idx[t + 2]);
    }

    size_t largest = 0;
    for (const auto &kv : byRoot)
      largest = std::max(largest, kv.second.size());
    const size_t floorTris =
        static_cast<size_t>(static_cast<float>(largest) * minFraction);

    for (const auto &kv : byRoot) {
      if (kv.second.size() < floorTris || kv.second.size() < 3)
        continue;
      MeshData piece;
      piece.sourcePath = mesh.sourcePath;
      MeshSubmeshData dst;
      dst.material = sm.material;
      std::unordered_map<uint32_t, uint32_t> remap;
      remap.reserve(kv.second.size());
      for (uint32_t old : kv.second) {
        auto it = remap.find(old);
        if (it == remap.end()) {
          const uint32_t fresh = static_cast<uint32_t>(dst.vertices.size());
          dst.vertices.push_back(sm.vertices[old]);
          it = remap.emplace(old, fresh).first;
        }
        dst.indices.push_back(it->second);
      }
      // MeshData::getGlobalBounds SKIPS any submesh with hasBounds false, and
      // returns false if that leaves it with nothing -- which makes both
      // recenter() and the normalize step below silent no-ops. A freshly
      // built submesh has to fill these in itself.
      dst.aabbMin = glm::vec3(1e30f);
      dst.aabbMax = glm::vec3(-1e30f);
      for (const MeshVertex &v : dst.vertices) {
        dst.aabbMin = glm::min(dst.aabbMin, v.pos);
        dst.aabbMax = glm::max(dst.aabbMax, v.pos);
      }
      dst.hasBounds = !dst.vertices.empty();

      piece.submeshes.push_back(std::move(dst));
      out.push_back(std::move(piece));
    }
  }
  // Biggest piece first, so the output numbering is stable and _0 is the one
  // worth looking at.
  std::sort(out.begin(), out.end(), [](const MeshData &a, const MeshData &b) {
    return triangleCount(a) > triangleCount(b);
  });
  return out;
}

bool writeObj(const MeshData &mesh, const std::string &path,
              const std::string &materialName) {
  std::ofstream f(path);
  if (!f) {
    std::fprintf(stderr, "glgen-bake: cannot write '%s'\n", path.c_str());
    return false;
  }
  f << "# baked by glgen-bake\n";
  if (!materialName.empty()) {
    f << "mtllib " << std::filesystem::path(path).stem().string() << ".mtl\n";
  }
  f << "o baked\n";

  // OBJ indices are 1-based and global across the file, so each submesh's
  // block is offset by everything written before it.
  size_t base = 1;
  for (const MeshSubmeshData &sm : mesh.submeshes) {
    for (const MeshVertex &v : sm.vertices)
      f << "v " << v.pos.x << ' ' << v.pos.y << ' ' << v.pos.z << '\n';
    for (const MeshVertex &v : sm.vertices)
      f << "vt " << v.uv.x << ' ' << v.uv.y << '\n';
    for (const MeshVertex &v : sm.vertices)
      f << "vn " << v.normal.x << ' ' << v.normal.y << ' ' << v.normal.z << '\n';
    if (!materialName.empty())
      f << "usemtl " << materialName << '\n';

    auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
      const size_t ia = base + a, ib = base + b, ic = base + c;
      f << "f " << ia << '/' << ia << '/' << ia << ' ' << ib << '/' << ib << '/'
        << ib << ' ' << ic << '/' << ic << '/' << ic << '\n';
    };
    if (sm.indices.empty()) {
      for (size_t i = 0; i + 2 < sm.vertices.size(); i += 3)
        emit(static_cast<uint32_t>(i), static_cast<uint32_t>(i + 1),
             static_cast<uint32_t>(i + 2));
    } else {
      for (size_t i = 0; i + 2 < sm.indices.size(); i += 3)
        emit(sm.indices[i], sm.indices[i + 1], sm.indices[i + 2]);
    }
    base += sm.vertices.size();
  }
  return true;
}

void applyScale(MeshData &mesh, float s) {
  if (std::abs(s - 1.0f) < 1e-6f)
    return;
  for (MeshSubmeshData &sm : mesh.submeshes) {
    for (MeshVertex &v : sm.vertices)
      v.pos *= s;
    sm.aabbMin *= s;
    sm.aabbMax *= s;
  }
  for (auto &ob : mesh.objectBounds) {
    ob.second.aabbMin *= s;
    ob.second.aabbMax *= s;
  }
}

// Writes the .mtl the baked OBJ's `mtllib` line names. Texture paths are
// emitted as given; the OBJ parser resolves them relative to the .mtl's own
// directory (and also probes a `textures/` subdirectory), so a bare filename
// alongside the mesh is the portable choice.
bool writeMtl(const std::string &objPath, const std::string &materialName,
              const Options &o) {
  if (materialName.empty() ||
      (o.albedoTex.empty() && o.normalTex.empty() && o.roughnessTex.empty()))
    return true;
  std::filesystem::path p(objPath);
  p.replace_extension(".mtl");
  std::ofstream f(p);
  if (!f) {
    std::fprintf(stderr, "glgen-bake: cannot write '%s'\n", p.string().c_str());
    return false;
  }
  f << "# baked by glgen-bake\n";
  f << "newmtl " << materialName << "\n";
  f << "Kd 1.000000 1.000000 1.000000\n";
  f << "d 1.000000\n";
  f << "illum 2\n";
  if (!o.albedoTex.empty())
    f << "map_Kd " << o.albedoTex << "\n";
  if (!o.normalTex.empty())
    f << "norm " << o.normalTex << "\n";
  if (!o.roughnessTex.empty())
    f << "map_Pr " << o.roughnessTex << "\n";
  return true;
}

void recenter(MeshData &mesh, const std::string &mode) {
  if (mode == "baseY")
    mesh.recenter(MeshData::Recenter::BaseY);
  else if (mode == "center")
    mesh.recenter(MeshData::Recenter::Center);
}

} // namespace

int main(int argc, char **argv) {
  Options o;
  if (!parseArgs(argc, argv, o))
    usage(2);

  std::printf("[bake] parsing %s\n", o.in.c_str());
  std::unique_ptr<MeshData> mesh = parseMeshFile(o.in);
  if (!mesh) {
    std::fprintf(stderr, "glgen-bake: failed to parse '%s'\n", o.in.c_str());
    return 1;
  }
  std::printf("[bake] source: %zu triangles, %zu submeshes\n",
              triangleCount(*mesh), mesh->submeshes.size());

  if (glm::dot(o.rotateDeg, o.rotateDeg) > 1e-8f)
    mesh->rotateEulerDeg(o.rotateDeg);

  if (o.normalizeHeight > 0.0f && !o.normalizeEach) {
    glm::vec3 mn, mx;
    if (mesh->getGlobalBounds(mn, mx) && (mx.y - mn.y) > 1e-6f) {
      const float s = o.normalizeHeight / (mx.y - mn.y);
      applyScale(*mesh, s);
      std::printf("[bake] normalized height %.4f -> %.2f m (scale %.4f)\n",
                  mx.y - mn.y, o.normalizeHeight, s);
    }
  }

  std::vector<MeshData> pieces;
  if (o.split) {
    pieces = splitComponents(*mesh, o.minComponent);
    std::printf("[bake] split into %zu components (>= %.1f%% of the largest)\n",
                pieces.size(), o.minComponent * 100.0f);
    if (pieces.empty())
      pieces.push_back(std::move(*mesh));
  } else {
    pieces.push_back(std::move(*mesh));
  }

  const std::filesystem::path outPath(o.out);
  size_t written = 0;
  for (size_t i = 0; i < pieces.size(); ++i) {
    if (!o.keep.empty() &&
        std::find(o.keep.begin(), o.keep.end(), i) == o.keep.end())
      continue;
    MeshData &piece = pieces[i];

    if (o.normalizeHeight > 0.0f && o.normalizeEach) {
      glm::vec3 mn, mx;
      if (piece.getGlobalBounds(mn, mx) && (mx.y - mn.y) > 1e-6f)
        applyScale(piece, o.normalizeHeight / (mx.y - mn.y));
    }

    const size_t before = triangleCount(piece);
    if (o.maxTris > 0 && before > o.maxTris)
      gen::decimateMesh(piece, o.maxTris);
    // Recenter LAST: decimation can remove the extreme vertices that defined
    // the footprint, so a pivot computed before it drifts off the base.
    recenter(piece, o.recenter);

    // Name from --names if supplied (positionally, against --keep), else the
    // component index appended to the --out stem.
    std::string stem = outPath.stem().string();
    if (!o.keep.empty() && !o.names.empty()) {
      const size_t slot = static_cast<size_t>(
          std::find(o.keep.begin(), o.keep.end(), i) - o.keep.begin());
      if (slot < o.names.size())
        stem = o.names[slot];
      else
        stem += "_" + std::to_string(i);
    } else if (pieces.size() > 1) {
      stem += "_" + std::to_string(i);
    }
    const std::filesystem::path dst =
        outPath.parent_path() / (stem + outPath.extension().string());

    if (!writeObj(piece, dst.string(), o.material))
      return 1;
    if (!writeMtl(dst.string(), o.material, o))
      return 1;
    ++written;

    glm::vec3 mn, mx;
    piece.getGlobalBounds(mn, mx);
    std::printf("[bake] %s: %zu -> %zu tris, extent %.2f x %.2f x %.2f m\n",
                dst.filename().string().c_str(), before, triangleCount(piece),
                mx.x - mn.x, mx.y - mn.y, mx.z - mn.z);
  }
  if (written == 0) {
    std::fprintf(stderr, "glgen-bake: nothing written (check --keep)\n");
    return 1;
  }
  return 0;
}
