#include "ufbx.h"
#include "json.hpp"
#include <cmath>
#include <cstdio>
#include <string>
#include <fstream>
#include <filesystem>
#include <map>
#include <vector>
using json=nlohmann::json;
struct Part { std::vector<float> p,n,uv; ufbx_vec3 pivot{}; };
int main(int argc,char **argv) {
  if(argc<3)return 1;
  ufbx_load_opts opts{};opts.target_axes=ufbx_axes_right_handed_y_up;opts.target_unit_meters=1;
  ufbx_error error{};auto *scene=ufbx_load_file(argv[1],&opts,&error);
  if(!scene){char msg[1024];ufbx_format_error(msg,sizeof(msg),&error);puts(msg);return 2;}
  std::map<std::string,Part> parts;
  for(auto *node:scene->nodes){
    auto *mesh=node->mesh;if(!mesh)continue;
    auto *skin=mesh->skin_deformers.count?mesh->skin_deformers[0]:nullptr;
    std::vector<uint32_t> indices(mesh->max_face_triangles*3);
    for(auto face:mesh->faces){
      auto count=ufbx_triangulate_face(indices.data(),indices.size(),mesh,face);
      for(uint32_t t=0;t<count;++t){
        std::string name="body";ufbx_node *bone=nullptr;
        if(skin){
          const auto v=skin->vertices[mesh->vertex_indices[indices[t*3]]];
          if(v.num_weights)bone=skin->clusters[skin->weights[v.weight_begin].cluster_index]->bone_node;
        }
        if(bone){
          std::string b=bone->name.data;
          if(b=="MAGAZINE"||b=="SLIDE"||b=="TRIGGER"||b=="SELECTOR")name=b;
        }
        auto &part=parts[name];
        if(name!="body")part.pivot=bone->node_to_world.cols[3];
        for(int k=0;k<3;++k){
          auto ix=indices[t*3+k];
          auto p=ufbx_transform_position(&node->geometry_to_world,ufbx_get_vertex_vec3(&mesh->vertex_position,ix));
          auto n=ufbx_transform_direction(&node->geometry_to_world,ufbx_get_vertex_vec3(&mesh->vertex_normal,ix));
          double len=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);if(len<1e-12)len=1;
          auto uv=ufbx_get_vertex_vec2(&mesh->vertex_uv,ix);
          // Bake a half turn: glGen viewmodels face -Z, source FBX faces +Z.
          part.p.insert(part.p.end(),{float(-(p.x-part.pivot.x)),float(p.y-part.pivot.y),float(-(p.z-part.pivot.z))});
          part.n.insert(part.n.end(),{float(-n.x/len),float(n.y/len),float(-n.z/len)});
          part.uv.insert(part.uv.end(),{float(uv.x),float(1-uv.y)});
        }
      }
    }
  }
  std::filesystem::path out=argv[2];std::filesystem::create_directories(out);
  json metadata={{"source","Stein Games Classic Weapons Pack v1.1"},{"parts",json::array()},{"muzzle",{0,.0957,-.7006}},{"ejection",{.0265,.1191,-.1893}}};
  json assembly={{"asset",{{"version","2.0"}}},{"scenes",json::array({{{"nodes",json::array()}}})},{"scene",0},{"nodes",json::array()},{"meshes",json::array()},{"buffers",json::array()},{"bufferViews",json::array()},{"accessors",json::array()}};
  auto textures=[](json &g){
    g["materials"]=json::parse(R"({"name":"AK47","pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":2},"metallicFactor":1,"roughnessFactor":1},"normalTexture":{"index":1},"occlusionTexture":{"index":2}})");
    g["materials"]=json::array({g["materials"]});
    g["images"]=json::parse(R"([{"uri":"T_AK47_C.png"},{"uri":"normal_gl.png"},{"uri":"orm.png"}])");
    g["textures"]=json::parse(R"([{"source":0},{"source":1},{"source":2}])");
  };
  textures(assembly);
  for(auto &[name,part]:parts){
    const auto file=name+".bin";std::ofstream bin(out/file,std::ios::binary);
    for(auto *v:{&part.p,&part.n,&part.uv})bin.write((const char*)v->data(),v->size()*sizeof(float));
    json g={{"asset",{{"version","2.0"}}},{"scenes",json::array({{{"nodes",{0}}}})},{"scene",0},{"nodes",json::array({{{"mesh",0},{"name",name}}})},{"buffers",json::array({{{"uri",file},{"byteLength",(part.p.size()+part.n.size()+part.uv.size())*4}}})},{"bufferViews",json::array()},{"accessors",json::array()}};
    size_t offset=0;
    for(auto *v:{&part.p,&part.n,&part.uv}){int i=g["bufferViews"].size();g["bufferViews"].push_back({{"buffer",0},{"byteOffset",offset},{"byteLength",v->size()*4},{"target",34962}});g["accessors"].push_back({{"bufferView",i},{"componentType",5126},{"count",part.p.size()/3},{"type",i==2?"VEC2":"VEC3"}});offset+=v->size()*4;}
    float mn[3]={1e9f,1e9f,1e9f},mx[3]={-1e9f,-1e9f,-1e9f};
    for(size_t i=0;i<part.p.size();++i){mn[i%3]=std::min(mn[i%3],part.p[i]);mx[i%3]=std::max(mx[i%3],part.p[i]);}
    g["accessors"][0]["min"]={mn[0],mn[1],mn[2]};g["accessors"][0]["max"]={mx[0],mx[1],mx[2]};
    g["meshes"]=json::parse(R"([{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"material":0}]}])");textures(g);
    std::ofstream(out/(name+".gltf"))<<g.dump(2);
    json pivot={-part.pivot.x,part.pivot.y,-part.pivot.z};
    metadata["parts"].push_back({{"name",name},{"file",name+".gltf"},{"pivot",pivot},{"triangles",part.p.size()/9}});
    printf("%s: %zu triangles, bounds %.4f %.4f %.4f / %.4f %.4f %.4f\n",name.c_str(),part.p.size()/9,mn[0],mn[1],mn[2],mx[0],mx[1],mx[2]);
    int base=assembly["accessors"].size(),buffer=assembly["buffers"].size();
    assembly["buffers"].push_back(g["buffers"][0]);
    for(int i=0;i<3;++i){auto bv=g["bufferViews"][i];bv["buffer"]=buffer;assembly["bufferViews"].push_back(bv);auto a=g["accessors"][i];a["bufferView"]=base+i;assembly["accessors"].push_back(a);}
    auto mesh=g["meshes"][0];mesh["primitives"][0]["attributes"]={{"POSITION",base},{"NORMAL",base+1},{"TEXCOORD_0",base+2}};
    int index=assembly["meshes"].size();assembly["meshes"].push_back(mesh);assembly["nodes"].push_back({{"mesh",index},{"name",name},{"translation",pivot}});assembly["scenes"][0]["nodes"].push_back(index);
  }
  std::ofstream(out/"rig.json")<<metadata.dump(2);std::ofstream(out/"ak47.gltf")<<assembly.dump(2);ufbx_free_scene(scene);
}
