#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace SK::Renderer
{
    /*
        Global GPU Types shared by renderer frontends and backends.
    */

    // CPU side reflection of the global scene uniform buffer used by shaders.
    struct GPUSceneData
    {
        glm::mat4 view;
        glm::mat4 proj;
        glm::mat4 viewproj;
        glm::vec4 camPos;
        glm::vec4 ambientColor;
        glm::vec4 sunlightDirection; // w for sun power
        glm::vec4 sunlightColor;
    };

    // Layout of vertex (storage) buffer
    struct Vertex
    {
        glm::vec3 position;
        float uv_x;
        glm::vec3 normal;
        float uv_y;
        glm::vec4 tangent;
    };
};