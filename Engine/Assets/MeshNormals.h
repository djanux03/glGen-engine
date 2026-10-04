#pragma once
#include "MeshData.h"
#include <cmath>

// Recover missing/invalid normals without changing topology, UV seams, or
// authored normals. Imports can legally omit NORMAL; some exporters also
// write an all-zero accessor, which previously made whole assets shade black.
inline size_t repairMissingNormals(MeshData &mesh) {
  size_t repaired=0;
  for(auto &submesh:mesh.submeshes) {
    const size_t count=submesh.vertices.size();
    std::vector<bool> missing(count,false);
    std::vector<glm::vec3> sum(count,glm::vec3(0));
    for(size_t i=0;i<count;++i) {
      float length2=glm::dot(submesh.vertices[i].normal,submesh.vertices[i].normal);
      missing[i]=!std::isfinite(length2)||length2<1e-12f;
    }
    const size_t indices=submesh.indices.empty()?count:submesh.indices.size();
    for(size_t i=0;i+2<indices;i+=3) {
      size_t a=submesh.indices.empty()?i:submesh.indices[i];
      size_t b=submesh.indices.empty()?i+1:submesh.indices[i+1];
      size_t c=submesh.indices.empty()?i+2:submesh.indices[i+2];
      if(a>=count||b>=count||c>=count)continue;
      const auto normal=glm::cross(submesh.vertices[b].pos-submesh.vertices[a].pos,
                                   submesh.vertices[c].pos-submesh.vertices[a].pos);
      if(!std::isfinite(glm::dot(normal,normal)))continue;
      // Unnormalized cross products weight shared vertices by triangle area.
      if(missing[a])sum[a]+=normal;
      if(missing[b])sum[b]+=normal;
      if(missing[c])sum[c]+=normal;
    }
    for(size_t i=0;i<count;++i)if(missing[i]) {
      float length2=glm::dot(sum[i],sum[i]);
      submesh.vertices[i].normal=std::isfinite(length2)&&length2>1e-12f?
          sum[i]/std::sqrt(length2):glm::vec3(0,1,0);
      ++repaired;
    }
  }
  return repaired;
}
