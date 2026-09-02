#include "MeshExportGLTF.h"
#include "tiny_gltf.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace {
template <class T> size_t append(std::vector<unsigned char> &out, const std::vector<T> &src) {
  while (out.size() % 4) out.push_back(0);
  const size_t offset = out.size();
  const size_t bytes = src.size() * sizeof(T);
  out.resize(offset + bytes);
  if (bytes) std::memcpy(out.data() + offset, src.data(), bytes);
  return offset;
}
int view(tinygltf::Model &m, size_t offset, size_t length, int target) {
  tinygltf::BufferView v; v.buffer=0; v.byteOffset=offset; v.byteLength=length; v.target=target;
  m.bufferViews.push_back(v); return static_cast<int>(m.bufferViews.size()-1);
}
int accessor(tinygltf::Model &m,int bufferView,int component,int type,size_t count,size_t offset=0) {
  tinygltf::Accessor a; a.bufferView=bufferView;a.byteOffset=offset;a.componentType=component;a.type=type;a.count=count;
  m.accessors.push_back(a);return static_cast<int>(m.accessors.size()-1);
}
} // namespace

bool exportMeshGLB(const MeshData &mesh, const std::string &path, std::string &error) {
  if (mesh.submeshes.empty()) { error="cannot export an empty mesh"; return false; }
  tinygltf::Model model; model.asset.version="2.0"; model.asset.generator="glGen character runtime";
  model.buffers.emplace_back(); tinygltf::Mesh outMesh; outMesh.name="glGenAsset";
  // MeshData carries decoded procedural maps, not filesystem paths.  Hand the
  // raw pixels to tinygltf so it PNG-encodes and embeds them in the GLB; the
  // exported character therefore remains portable after its job folder moves.
  std::unordered_map<std::string, int> textures;
  for (size_t i=0; i<mesh.images.size(); ++i) {
    const MeshImage &src=mesh.images[i];
    if (src.pixels.empty() || src.width <= 0 || src.height <= 0) continue;
    tinygltf::Image image; image.name=src.key; image.uri="glgen_image_"+std::to_string(i)+".png";
    image.width=src.width; image.height=src.height; image.component=src.component; image.bits=8;
    image.pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE; image.image=src.pixels;
    model.images.push_back(std::move(image)); tinygltf::Texture texture; texture.source=static_cast<int>(model.images.size()-1);
    model.textures.push_back(texture); textures.emplace(src.key, static_cast<int>(model.textures.size()-1));
  }
  for (const MeshSubmeshData &sm : mesh.submeshes) {
    if (sm.vertices.empty()) continue;
    std::vector<float> positions, normals, uvs; positions.reserve(sm.vertices.size()*3); normals.reserve(sm.vertices.size()*3); uvs.reserve(sm.vertices.size()*2);
    glm::vec3 mn(1e30f),mx(-1e30f);
    for(const MeshVertex &v:sm.vertices){positions.insert(positions.end(),{v.pos.x,v.pos.y,v.pos.z});normals.insert(normals.end(),{v.normal.x,v.normal.y,v.normal.z});uvs.insert(uvs.end(),{v.uv.x,v.uv.y});mn=glm::min(mn,v.pos);mx=glm::max(mx,v.pos);}
    std::vector<uint32_t> indices=sm.indices; if(indices.empty()){indices.resize(sm.vertices.size());for(uint32_t i=0;i<indices.size();++i)indices[i]=i;}
    const size_t po=append(model.buffers[0].data,positions), no=append(model.buffers[0].data,normals), uo=append(model.buffers[0].data,uvs), io=append(model.buffers[0].data,indices);
    const int pv=view(model,po,positions.size()*sizeof(float),TINYGLTF_TARGET_ARRAY_BUFFER), nv=view(model,no,normals.size()*sizeof(float),TINYGLTF_TARGET_ARRAY_BUFFER), uv=view(model,uo,uvs.size()*sizeof(float),TINYGLTF_TARGET_ARRAY_BUFFER), iv=view(model,io,indices.size()*sizeof(uint32_t),TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
    const int pa=accessor(model,pv,TINYGLTF_COMPONENT_TYPE_FLOAT,TINYGLTF_TYPE_VEC3,sm.vertices.size()); model.accessors[pa].minValues={mn.x,mn.y,mn.z};model.accessors[pa].maxValues={mx.x,mx.y,mx.z};
    tinygltf::Material material;material.name=sm.material.id;material.pbrMetallicRoughness.baseColorFactor={sm.material.baseColor.r,sm.material.baseColor.g,sm.material.baseColor.b,sm.material.baseColor.a};material.pbrMetallicRoughness.metallicFactor=sm.material.metallic;material.pbrMetallicRoughness.roughnessFactor=sm.material.roughness;
    const auto diffuse=textures.find(sm.material.texDiffusePath); if(diffuse!=textures.end()) material.pbrMetallicRoughness.baseColorTexture.index=diffuse->second;
    const auto normal=textures.find(sm.material.texNormalPath); if(normal!=textures.end()) material.normalTexture.index=normal->second;
    const auto ao=textures.find(sm.material.texAOPath); if(ao!=textures.end()) material.occlusionTexture.index=ao->second;
    model.materials.push_back(material);
    tinygltf::Primitive p;p.attributes["POSITION"]=pa;p.attributes["NORMAL"]=accessor(model,nv,TINYGLTF_COMPONENT_TYPE_FLOAT,TINYGLTF_TYPE_VEC3,sm.vertices.size());p.attributes["TEXCOORD_0"]=accessor(model,uv,TINYGLTF_COMPONENT_TYPE_FLOAT,TINYGLTF_TYPE_VEC2,sm.vertices.size());p.indices=accessor(model,iv,TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,TINYGLTF_TYPE_SCALAR,indices.size());p.material=static_cast<int>(model.materials.size()-1);p.mode=TINYGLTF_MODE_TRIANGLES;outMesh.primitives.push_back(std::move(p));
  }
  if(outMesh.primitives.empty()){error="mesh has no exportable primitives";return false;} model.meshes.push_back(std::move(outMesh));tinygltf::Node node;node.mesh=0;model.nodes.push_back(node);tinygltf::Scene scene;scene.nodes.push_back(0);model.scenes.push_back(scene);model.defaultScene=0;
  std::error_code ec;const std::filesystem::path output(path);if(output.has_parent_path())std::filesystem::create_directories(output.parent_path(),ec);
  tinygltf::TinyGLTF writer; if(!writer.WriteGltfSceneToFile(&model,path,true,true,false,true)){error="tinygltf could not write '"+path+"'";return false;}return true;
}
