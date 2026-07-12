#pragma once

class AssetManager;

// Installs the OpenGL GPU-upload backend (OBJModel/FBXModel/UFBXModel) and
// the GL shader hot-reloader on an AssetManager. Call once at startup,
// before any assets are loaded.
void InstallGLModelBackend(AssetManager &assets);
