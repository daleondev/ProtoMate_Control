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
    enum class UpAxis : std::uint8_t
    {
        Y,
        Z
    };
    struct ModelCoordinates
    {
        // Coordinates AFTER Assimp import. COLLADA Z_UP is normalized to Y-up
        // by Assimp; these metre exports therefore need the default conversion.
        static constexpr double MILLIMETRES_PER_METRE{ 1000 };
        double millimetres_per_unit{ MILLIMETRES_PER_METRE };
        UpAxis up{ UpAxis::Y };
        glm::mat4 matrix() const;
    };
    struct ModelGeometry
    {
        std::vector<Vertex> vertices{};
        std::vector<std::uint32_t> indices{};
        static constexpr glm::vec4 DEFAULT_COLOR{ 0.7F, 0.7F, 0.7F, 1.0F };
        glm::vec4 color{ DEFAULT_COLOR };
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
    // Joints are relative rotary radians and Z displacement from CAD rest in millimetres.
    std::array<glm::mat4, 4> robot_model_transforms(const Profile& profile,
                                                    const std::array<double, 3>& joints);
}
