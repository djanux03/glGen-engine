#pragma once
#include <string>
#include <glad/glad.h>

enum class TextureUsage {
  Color,
  Data,
};

GLuint LoadTexture2D(const std::string& path, bool flipY = true,
                     TextureUsage usage = TextureUsage::Color);
GLuint LoadTexture2DCached(const std::string& path, bool flipY = true,
                           TextureUsage usage = TextureUsage::Color);
GLuint LoadHDRTexture2D(const std::string& path, bool flipY = true);
