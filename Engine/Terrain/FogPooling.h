#pragma once
#include <algorithm>
#include <cmath>
#include <queue>
#include <vector>
#include <cstdint>

namespace atmosphere {
// Priority-Flood (Barnes, Lehman and Mulla, 2014) gives each depression its
// lowest spill elevation. This is a terrain potential for cold-air pooling;
// it does not modify terrain, water, or claim to simulate weather dynamics.
inline std::vector<float> basinDepths(const std::vector<float>& height,int width,int heightRows) {
  if(width<1||heightRows<1||height.size()!=size_t(width)*heightRows)return {};
  struct Cell {float level;int index;};
  struct Compare {bool operator()(Cell a,Cell b) const {return a.level!=b.level?a.level>b.level:a.index>b.index;}};
  std::priority_queue<Cell,std::vector<Cell>,Compare> queue;
  std::vector<uint8_t> visited(height.size(),0);std::vector<float> depth(height.size(),0);
  auto seed=[&](int index){if(!visited[index]){visited[index]=1;queue.push({height[index],index});}};
  for(int z=0;z<heightRows;++z)for(int x=0;x<width;++x) {
    const int index=z*width+x;
    if(!std::isfinite(height[index])){visited[index]=1;continue;}
    bool boundary=x==0||z==0||x==width-1||z==heightRows-1;
    for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
      const int nx=x+dx,nz=z+dz;
      if(nx>=0&&nx<width&&nz>=0&&nz<heightRows&&!std::isfinite(height[nz*width+nx]))boundary=true;
    }
    if(boundary)seed(index); // absent data is an open boundary, not a dam
  }
  while(!queue.empty()) {
    const Cell cell=queue.top();queue.pop();const int x=cell.index%width,z=cell.index/width;
    for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
      const int nx=x+dx,nz=z+dz;if(nx<0||nx>=width||nz<0||nz>=heightRows)continue;
      const int index=nz*width+nx;if(visited[index])continue;visited[index]=1;
      const float spill=std::max(cell.level,height[index]);depth[index]=spill-height[index];queue.push({spill,index});
    }
  }
  return depth;
}
} // namespace atmosphere
