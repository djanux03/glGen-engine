# glGen Engine

Modern C++ Game Engine built on OpenGL 4.1.

<img width="1465" height="823" alt="Screenshot 2026-02-20 at 18 33 22" src="https://github.com/user-attachments/assets/943b84f3-85d9-400f-96eb-64659377a797" />
png)

## Overview


**glGen** is a lightweight, high-performance engine focusing on architectural clarity and performance.

<img width="1462" height="825" alt="Screenshot 2026-02-20 at 18 42 12" src="https://github.com/user-attachments/assets/2baf568c-4952-4acb-81f2-6f1ca993eb2b" />

## Core Capabilities

- **Custom ECS**: Efficient Entity-Component-System with sparse-set storage.
- **Advanced Rendering**: Directional shadows, HDR lighting, and procedural clouds.
- **Integrated Editor**: Real-time scene manipulation using ImGui/ImGuizmo.
- **FBX/OBJ Support**: Native asset loading through specialized loaders.
- **Jolt Physics**: High-performance rigid body physics with debug rendering and interactive collider gizmos.

## Physics & Colliders

glGen integrates the **Jolt Physics** library for robust rigid body simulations. It features interactive collider editing directly in the viewport.

- **Interactive Gizmos**: Manipulate collider dimensions independently of the entity transform.
- **Debug Rendering**: Real-time wireframe visualization of collision shapes (Static, Dynamic, and Kinematic).

<img width="800" alt="Physics Debug Placeholder" src="https://via.placeholder.com/800x400?text=Insert+Physics+Debug+Screenshot+Here" />

## Lua Scripting

glGen features a seamlessly integrated Lua scripting environment powered by `sol2`. You can write, attach, and execute scripts dynamically to control entities and game logic.

- **Built-in Script Editor**: A fully-featured, dockable window directly inside the editor.
- **Hot-Reloading**: Instantly test changes—scripts reload automatically upon saving.


<!-- Add your Script Editor / Lua screenshot here -->
<img width="800" alt="Script Editor Placeholder" src="https://via.placeholder.com/800x400?text=Insert+Script+Editor+Screenshot+Here" />

## Getting Started

### Prerequisites
- C++17
- OpenGL 4.1+
- CMake

### Build & Run
```bash
cmake -B Build
cmake --build Build
./run.sh
```
<img width="1467" height="823" alt="Screenshot 2026-02-20 at 18 52 21" src="https://github.com/user-attachments/assets/34ba8f96-b4d0-4fbe-85f3-db3e29b0e771" />
