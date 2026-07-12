#include "Texture.h"
#include "Logger.h"
#include <stb/stb_image.h>
#include <unordered_map>

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

namespace {
std::unordered_map<std::string, GLuint> gTextureCache;

std::string makeCacheKey(const std::string &path, bool flipY, TextureUsage usage) {
    return path + "|" + (flipY ? "flip" : "noflip") + "|" +
           (usage == TextureUsage::Color ? "color" : "data");
}

bool hasExtension(const char *extName)
{
    if (!extName || !*extName)
        return false;

    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    if (major >= 3 && glGetStringi != nullptr) {
        GLint extCount = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &extCount);
        for (GLint i = 0; i < extCount; ++i) {
            const char *ext =
                reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, (GLuint)i));
            if (ext && std::strcmp(ext, extName) == 0)
                return true;
        }
        return false;
    }

    const char *exts = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
    if (!exts)
        return false;
    return std::string(exts).find(extName) != std::string::npos;
}

void applyAnisotropicFiltering(GLuint tex)
{
    if (tex == 0)
        return;

    if (!hasExtension("GL_EXT_texture_filter_anisotropic")) {
        return;
    }

    GLfloat maxAniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
    if (maxAniso <= 1.0f)
        return;

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                    std::min(maxAniso, 8.0f));
}
}

GLuint LoadTexture2D(const std::string& path, bool flipY, TextureUsage usage)
{
    stbi_set_flip_vertically_on_load(flipY);

    int w, h, channels;
    
    // FIX: Force '4' as the last argument to request RGBA data.
    // This ensures every pixel is 4 bytes, preventing memory alignment issues
    // that cause "unloadable texture" errors on MacOS.
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    
    if (!data) {
        LOG_ERROR("Asset", "Failed to load texture: " + path +
                               " reason: " +
                               (stbi_failure_reason() ? stbi_failure_reason()
                                                      : "unknown"));
        return 0;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    // GL_REPEAT is usually better for 3D models than CLAMP_TO_EDGE
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    const GLenum internalFormat =
        (usage == TextureUsage::Color) ? GL_SRGB_ALPHA : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    applyAnisotropicFiltering(tex);

    stbi_image_free(data);
    return tex;
}

GLuint LoadTexture2DCached(const std::string& path, bool flipY, TextureUsage usage)
{
    if (path.empty())
        return 0;

    const std::string key = makeCacheKey(path, flipY, usage);
    auto it = gTextureCache.find(key);
    if (it != gTextureCache.end())
        return it->second;

    GLuint tex = LoadTexture2D(path, flipY, usage);
    if (tex != 0)
        gTextureCache[key] = tex;
    return tex;
}

GLuint CreateTexture2DFromPixels(const unsigned char* pixels, int width,
                                 int height, int component,
                                 TextureUsage usage)
{
    if (!pixels || width <= 0 || height <= 0)
        return 0;

    GLuint texID = 0;
    glGenTextures(1, &texID);
    glBindTexture(GL_TEXTURE_2D, texID);

    GLenum format = GL_RGBA;
    GLenum internalFormat = GL_RGBA;
    if (component == 3) {
        format = GL_RGB;
        internalFormat = (usage == TextureUsage::Color) ? GL_SRGB : GL_RGB;
    } else if (component == 1) {
        format = GL_RED;
        internalFormat = GL_RED;
    } else {
        internalFormat =
            (usage == TextureUsage::Color) ? GL_SRGB_ALPHA : GL_RGBA;
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format,
                 GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);

    return texID;
}

GLuint LoadHDRTexture2D(const std::string& path, bool flipY)
{
    stbi_set_flip_vertically_on_load(flipY);

    int w, h, n;
    float* data = stbi_loadf(path.c_str(), &w, &h, &n, 0);
    if (!data)
    {
        LOG_ERROR("Asset", "Failed to load HDR: " + path +
                               " reason: " +
                               (stbi_failure_reason() ? stbi_failure_reason()
                                                      : "unknown"));
        return 0;
    }

    // HDR is fine as RGB usually, but if you have issues, you can check 'n'
    GLenum format = GL_RGB;
    if (n == 1) format = GL_RED;
    else if (n == 4) format = GL_RGBA;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); // Was GL_CLAMP_TO_EDGE?
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, w, h, 0, format, GL_FLOAT, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    applyAnisotropicFiltering(tex);

    stbi_image_free(data);
    return tex;
}
