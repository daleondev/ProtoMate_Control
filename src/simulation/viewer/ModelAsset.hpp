#pragma once
#include "../Profile.hpp"
#include "Geometry.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace simulation::viewer
{
    enum class UpAxis
    {
        Y,
        Z
    };
    struct ModelCoordinates
    {
        // glTF convention. OBJ/STL exports commonly need {1, UpAxis::Z} instead.
        double millimetres_per_unit{ 1000 };
        UpAxis up{ UpAxis::Y };
        glm::mat4 matrix() const;
    };
    struct ModelGeometry
    {
        std::vector<Vertex> vertices{};
        std::vector<std::uint32_t> indices{};
        glm::vec4 color{ 0.7F, 0.7F, 0.7F, 1.0F };
    };
    struct ModelInstance
    {
        std::size_t geometry{};
        glm::mat4 transform{ 1 };
        glm::vec3 center{};
    };
    // Owns copied CPU data; no Assimp pointers or GL context escape the import.
    struct ModelAsset
    {
        std::vector<ModelGeometry> geometries{};
        std::vector<ModelInstance> instances{};
        std::vector<std::string> warnings{};
        glm::vec3 minimum{}, maximum{};
        static ModelAsset read(const std::filesystem::path& file, ModelCoordinates coordinates = {});
        static ModelAsset read(std::span<const std::byte> bytes,
                               const char* format,
                               ModelCoordinates coordinates = {});
    };
    struct RobotModels
    {
        std::array<ModelAsset, 3> parts{};
        bool placeholders{ true };
        static RobotModels load(const std::filesystem::path& directory = {},
                                ModelCoordinates coordinates = {});
    };
    // Local frames: +X toward the next joint/TCP, +Z up; origins at the pivots.
    // Height is a visual mounting datum, independent of the simulated TCP Z.
    std::array<glm::mat4, 3> robotModelTransforms(const Profile& profile,
                                                  const std::array<double, 3>& joints,
                                                  double shoulder_height_mm = 140);
}
