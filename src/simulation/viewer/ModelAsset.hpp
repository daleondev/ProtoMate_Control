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
        // Coordinates AFTER Assimp import. COLLADA Z_UP is normalized to Y-up
        // by Assimp; these metre exports therefore need the default conversion.
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
        std::array<ModelAsset, 4> parts{};
        bool external{};
        static RobotModels load(const std::filesystem::path& directory = {},
                                ModelCoordinates coordinates = {});
    };
    // Base + three articulated DAE frames, registered against evaluated STEP.
    // Joints are relative rotary radians and world TCP height in millimetres.
    std::array<glm::mat4, 4> robotModelTransforms(const Profile& profile,
                                                  const std::array<double, 3>& joints);
}
