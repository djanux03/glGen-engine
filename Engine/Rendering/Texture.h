#pragma once
#include <string>
#include <glad/glad.h>

GLuint LoadTexture2D(const std::string& path, bool flipY = true);
GLuint LoadHDRTexture2D(const std::string& path, bool flipY = true);
