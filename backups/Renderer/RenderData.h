#pragma once

#include "Buffer.h"
#include "VertexArray.h"
#include "Shader.h"
#include "Texture.h"

//------------------------------------------------------
//                      Plane
//------------------------------------------------------

struct PlaneData
{
    struct Vertex
    {
        glm::vec3 position;
		glm::vec2 texCoord;
    };

    inline static const BufferLayout layout = {
        { ShaderDataType::Float3, "a_position" },
        { ShaderDataType::Float2, "a_texCoord" }
    };

    static constexpr const std::array<Vertex, 4> vertices = {
        Vertex{glm::vec3{-0.5f, -0.5f, 0.0f},    glm::vec2{0.0f, 0.0f}},
        Vertex{glm::vec3{ 0.5f, -0.5f, 0.0f},    glm::vec2{1.0f, 0.0f}},
        Vertex{glm::vec3{ 0.5f,  0.5f, 0.0f},    glm::vec2{1.0f, 1.0f}},
        Vertex{glm::vec3{-0.5f,  0.5f, 0.0f},    glm::vec2{0.0f, 1.0f}}
    };
    static constexpr const std::array<std::array<uint32_t, 3>, 2> indices = {
        std::array<uint32_t, 3>{0, 1, 2}, 
        std::array<uint32_t, 3>{2, 3, 0}
    };
};

//------------------------------------------------------
//                      Frame
//------------------------------------------------------

struct FrameData
{
    struct Vertex
    {
        glm::vec3 position;
		glm::vec4 color;
    };

    inline static const BufferLayout layout = {
        { ShaderDataType::Float3, "a_position" },
        { ShaderDataType::Float4, "a_color" }
    };

    static constexpr std::array<Vertex, 8 * 3> vertices = {
        Vertex{glm::vec3{-0.025f, -0.025f,  0.025f},      glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{ 1.0f,   -0.025f,  0.025f},      glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{ 1.0f,    0.025f,  0.025f},      glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{-0.025f,  0.025f,  0.025f},      glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{-0.025f, -0.025f, -0.025f},      glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{-0.025f,  0.025f, -0.025f},      glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{ 1.0f,   -0.025f, -0.025f},      glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{ 1.0f,    0.025f, -0.025f},      glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}},

        Vertex{glm::vec3{-0.025f, -0.025f,  0.025f},      glm::vec4{0.0f, 1.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{ 0.025f, -0.025f,  0.025f},      glm::vec4{0.0f, 1.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{ 0.025f,  1.0f,    0.025f},      glm::vec4{0.0f, 1.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{-0.025f,  1.0f,    0.025f},      glm::vec4{0.0f, 1.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{-0.025f, -0.025f, -0.025f},      glm::vec4{0.0f, 1.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{-0.025f,  1.0f,   -0.025f},      glm::vec4{0.0f, 1.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{ 0.025f, -0.025f, -0.025f},      glm::vec4{0.0f, 1.0f, 0.0f, 1.0f}},
        Vertex{glm::vec3{ 0.025f,  1.0f,   -0.025f},      glm::vec4{0.0f, 1.0f, 0.0f, 1.0f}},

        Vertex{glm::vec3{-0.025f, -0.025f,    1.0f},      glm::vec4{0.0f, 0.0f, 1.0f, 1.0f}},
        Vertex{glm::vec3{ 0.025f, -0.025f,    1.0f},      glm::vec4{0.0f, 0.0f, 1.0f, 1.0f}},
        Vertex{glm::vec3{ 0.025f,  0.025f,    1.0f},      glm::vec4{0.0f, 0.0f, 1.0f, 1.0f}},
        Vertex{glm::vec3{-0.025f,  0.025f,    1.0f},      glm::vec4{0.0f, 0.0f, 1.0f, 1.0f}},
        Vertex{glm::vec3{-0.025f, -0.025f, -0.025f},      glm::vec4{0.0f, 0.0f, 1.0f, 1.0f}},
        Vertex{glm::vec3{-0.025f,  0.025f, -0.025f},      glm::vec4{0.0f, 0.0f, 1.0f, 1.0f}},
        Vertex{glm::vec3{ 0.025f, -0.025f, -0.025f},      glm::vec4{0.0f, 0.0f, 1.0f, 1.0f}},
        Vertex{glm::vec3{ 0.025f,  0.025f, -0.025f},      glm::vec4{0.0f, 0.0f, 1.0f, 1.0f}}    
    };

    static constexpr std::array<std::array<uint32_t, 3>, 12 * 3> indices = {
            std::array<uint32_t, 3>{8 * 0 + 0, 8 * 0 + 1, 8 * 0 + 2},
            std::array<uint32_t, 3>{8 * 0 + 2, 8 * 0 + 3, 8 * 0 + 0},
            std::array<uint32_t, 3>{8 * 0 + 4, 8 * 0 + 0, 8 * 0 + 3},
            std::array<uint32_t, 3>{8 * 0 + 3, 8 * 0 + 5, 8 * 0 + 4},
            std::array<uint32_t, 3>{8 * 0 + 6, 8 * 0 + 4, 8 * 0 + 5},
            std::array<uint32_t, 3>{8 * 0 + 5, 8 * 0 + 7, 8 * 0 + 6},
            std::array<uint32_t, 3>{8 * 0 + 1, 8 * 0 + 6, 8 * 0 + 7},
            std::array<uint32_t, 3>{8 * 0 + 7, 8 * 0 + 2, 8 * 0 + 1},
            std::array<uint32_t, 3>{8 * 0 + 4, 8 * 0 + 6, 8 * 0 + 1},
            std::array<uint32_t, 3>{8 * 0 + 1, 8 * 0 + 0, 8 * 0 + 4},
            std::array<uint32_t, 3>{8 * 0 + 5, 8 * 0 + 7, 8 * 0 + 2},
            std::array<uint32_t, 3>{8 * 0 + 2, 8 * 0 + 3, 8 * 0 + 5},

            std::array<uint32_t, 3>{8 * 1 + 0, 8 * 1 + 1, 8 * 1 + 2},
            std::array<uint32_t, 3>{8 * 1 + 2, 8 * 1 + 3, 8 * 1 + 0},
            std::array<uint32_t, 3>{8 * 1 + 4, 8 * 1 + 0, 8 * 1 + 3},
            std::array<uint32_t, 3>{8 * 1 + 3, 8 * 1 + 5, 8 * 1 + 4},
            std::array<uint32_t, 3>{8 * 1 + 6, 8 * 1 + 4, 8 * 1 + 5},
            std::array<uint32_t, 3>{8 * 1 + 5, 8 * 1 + 7, 8 * 1 + 6},
            std::array<uint32_t, 3>{8 * 1 + 1, 8 * 1 + 6, 8 * 1 + 7},
            std::array<uint32_t, 3>{8 * 1 + 7, 8 * 1 + 2, 8 * 1 + 1},
            std::array<uint32_t, 3>{8 * 1 + 4, 8 * 1 + 6, 8 * 1 + 1},
            std::array<uint32_t, 3>{8 * 1 + 1, 8 * 1 + 0, 8 * 1 + 4},
            std::array<uint32_t, 3>{8 * 1 + 5, 8 * 1 + 7, 8 * 1 + 2},
            std::array<uint32_t, 3>{8 * 1 + 2, 8 * 1 + 3, 8 * 1 + 5},

            std::array<uint32_t, 3>{8 * 2 + 0, 8 * 2 + 1, 8 * 2 + 2},
            std::array<uint32_t, 3>{8 * 2 + 2, 8 * 2 + 3, 8 * 2 + 0},
            std::array<uint32_t, 3>{8 * 2 + 4, 8 * 2 + 0, 8 * 2 + 3},
            std::array<uint32_t, 3>{8 * 2 + 3, 8 * 2 + 5, 8 * 2 + 4},
            std::array<uint32_t, 3>{8 * 2 + 6, 8 * 2 + 4, 8 * 2 + 5},
            std::array<uint32_t, 3>{8 * 2 + 5, 8 * 2 + 7, 8 * 2 + 6},
            std::array<uint32_t, 3>{8 * 2 + 1, 8 * 2 + 6, 8 * 2 + 7},
            std::array<uint32_t, 3>{8 * 2 + 7, 8 * 2 + 2, 8 * 2 + 1},
            std::array<uint32_t, 3>{8 * 2 + 4, 8 * 2 + 6, 8 * 2 + 1},
            std::array<uint32_t, 3>{8 * 2 + 1, 8 * 2 + 0, 8 * 2 + 4},
            std::array<uint32_t, 3>{8 * 2 + 5, 8 * 2 + 7, 8 * 2 + 2},
            std::array<uint32_t, 3>{8 * 2 + 2, 8 * 2 + 3, 8 * 2 + 5}
    };
};

//------------------------------------------------------
//                      Sphere
//------------------------------------------------------

static constexpr float SPHERE_RADIUS = 0.5f;
static constexpr uint16_t SPHERE_RINGS = 16;
static constexpr uint16_t SPHERE_SECTORS = 32;

struct SphereData
{
    struct Vertex
    {
        glm::vec3 position;
    };

    inline static const BufferLayout layout = {
        { ShaderDataType::Float3, "a_position" }
    };

    static constexpr auto generateSphere() {
        constexpr size_t vertexCount = SPHERE_RINGS * SPHERE_SECTORS;
        constexpr size_t indexCount = (SPHERE_RINGS - 1) * SPHERE_SECTORS * 6;

        std::array<Vertex, vertexCount> vertices{};
        std::array<std::array<uint32_t, 3>, indexCount / 3> indices{};

        constexpr float R = 1.0f / static_cast<float>(SPHERE_RINGS - 1);
        constexpr float S = 1.0f / static_cast<float>(SPHERE_SECTORS - 1);

        // Generate vertices
        size_t vertexIndex = 0;
        for (uint16_t r = 0; r < SPHERE_RINGS; ++r) {
            for (uint16_t s = 0; s < SPHERE_SECTORS; ++s) {
                const float y = std::sin(-std::numbers::pi_v<float> / 2.0f + std::numbers::pi_v<float> * r * R);
                const float x = std::cos(2.0f * std::numbers::pi_v<float> * s * S) * std::sin(std::numbers::pi_v<float> * r * R);
                const float z = std::sin(2.0f * std::numbers::pi_v<float> * s * S) * std::sin(std::numbers::pi_v<float> * r * R);

                vertices[vertexIndex++].position = {x * SPHERE_RADIUS, y * SPHERE_RADIUS, z * SPHERE_RADIUS};
            }
        }

        // Generate indices for triangles
        size_t indexIndex = 0;
        for (uint16_t r = 0; r < SPHERE_RINGS - 1; ++r) {
            for (uint16_t s = 0; s < SPHERE_SECTORS; ++s) {
                uint16_t i1 = r * SPHERE_SECTORS + s;
                uint16_t i2 = r * SPHERE_SECTORS + (s + 1) % SPHERE_SECTORS;
                uint16_t i3 = (r + 1) * SPHERE_SECTORS + s;
                uint16_t i4 = (r + 1) * SPHERE_SECTORS + (s + 1) % SPHERE_SECTORS;

                indices[indexIndex++] = {i1, i2, i4};
                indices[indexIndex++] = {i1, i4, i3};
            }
        }

        return std::make_pair(vertices, indices);
    }
};

//------------------------------------------------------
//                      Mesh
//------------------------------------------------------

struct MeshData
{
    struct Vertex
    {
        glm::vec3 position;
		glm::vec4 color;
    };

    inline static const BufferLayout layout = {
        { ShaderDataType::Float3, "a_position" },
        { ShaderDataType::Float4, "a_color" }
    };

    std::vector<Vertex> vertices = {};
    std::vector<std::array<uint32_t, 3>> indices = {};
};

//------------------------------------------------------
//                      Box
//------------------------------------------------------

struct BoxData
{
    struct Vertex
    {
        glm::vec3 position;
    };

    inline static const BufferLayout layout = {
        { ShaderDataType::Float3, "a_position" }
    };

    std::array<glm::vec3, 8> vertices = {};
    static constexpr std::array<std::array<uint32_t, 3>, 12> indices = {
        std::array<uint32_t, 3>{0, 1, 2}, 
        std::array<uint32_t, 3>{0, 2, 3},
        std::array<uint32_t, 3>{4, 6, 5}, 
        std::array<uint32_t, 3>{4, 7, 6},
        std::array<uint32_t, 3>{0, 3, 7}, 
        std::array<uint32_t, 3>{0, 7, 4},
        std::array<uint32_t, 3>{1, 5, 6}, 
        std::array<uint32_t, 3>{1, 6, 2},
        std::array<uint32_t, 3>{3, 2, 6}, 
        std::array<uint32_t, 3>{3, 6, 7},
        std::array<uint32_t, 3>{0, 4, 5}, 
        std::array<uint32_t, 3>{0, 5, 1}
    };
};

//------------------------------------------------------
//                      Render Data
//------------------------------------------------------

struct RenderData
{
    std::shared_ptr<VertexArray> vertexArray;
    std::shared_ptr<VertexBuffer> vertexBuffer;
    std::shared_ptr<IndexBuffer> indexBuffer;
    std::shared_ptr<Shader> shader;
    std::shared_ptr<Texture2D> texture;
};