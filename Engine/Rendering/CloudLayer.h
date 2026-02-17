#pragma once
#include "OBJModel.h"
#include <glm/glm.hpp>
#include <string>

class Shader;

class CloudLayer
{
public:
    bool loadFromFile(const std::string& objPath);
    void shutdown();

    void draw(Shader& shader,
              const glm::vec3& position,
              const glm::vec3& scale,
              float alpha = 0.75f);   // global opacity multiplier

private:
    OBJModel mModel;
};
