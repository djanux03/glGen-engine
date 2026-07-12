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

// Creates a GL texture from already-decoded 8-bit pixel data (1, 3 or 4
// tightly packed channels). Used for embedded model textures.
GLuint CreateTexture2DFromPixels(const unsigned char* pixels, int width,
                                 int height, int component,
                                 TextureUsage usage = TextureUsage::Color);
