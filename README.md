# glGen Engine

<img width="1470" height="956" alt="Screenshot" src="https://github.com/user-attachments/assets/f883a43c-0529-48ff-9306-4571d3163c35" />
<img width="1470" height="922" alt="Screenshot" src="https://github.com/user-attachments/assets/ea39b52d-0dda-464a-bd43-25466a7cfa15" />

**glGen** is a C++ game engine build on OpenGL 4.1.

### Features
- **ECS**: Custom Entity-Component-System with compile-time IDs and sparse-set storage.
- **Renderer**: Directional lighting, shadow mapping (cubemaps), HDR skies, and infinite clouds.
- **Physics**: Projectile system with wake effects and ballistic simulation.
- **Editor**: Built-in UI using ImGui and ImGuizmo for live scene editing and entity outliner.
- **Assets**: Cached loading for OBJ models and textures.

### Optimization
The engine uses a data-oriented design to minimize driver overhead. Key optimizations include:
- Uniform location caching in the Shader class.
- Pass-level uniform batching (reducing per-object API calls).
- Unified ECS logic (removed legacy Scene entity system).

### Dependencies
- C++17
- OpenGL 4.1
- GLFW / Glad
- GLM
- ImGui / ImGuizmo

### Build
```bash
mkdir Build && cd Build
cmake ..
make
```
