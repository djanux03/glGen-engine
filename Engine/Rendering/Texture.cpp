#include "Texture.h"
#include "Logger.h"
#include <stb/stb_image.h>

GLuint LoadTexture2D(const std::string& path, bool flipY)
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

    // FIX: Always use GL_RGBA internal and format since we forced 4 channels above
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);

    stbi_image_free(data);
    return tex;
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, w, h, 0, format, GL_FLOAT, data);

    stbi_image_free(data);
    return tex;
}
