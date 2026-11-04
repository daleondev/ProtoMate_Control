#include "ModelAsset.hpp"
#include "../assets/EmbeddedAssets.hpp"
#include <algorithm>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cmath>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace simulation::viewer
{
    namespace
    {
        constexpr std::size_t MAX_VERTICES{ 2'000'000 }, MAX_INDICES{ 6'000'000 }, MAX_NODES{ 16384 };
        constexpr unsigned FLAGS{ aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                  aiProcess_GenSmoothNormals | aiProcess_ValidateDataStructure |
                                  aiProcess_ImproveCacheLocality };
        bool finite(glm::vec3 value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }
        glm::mat4 matrix(const aiMatrix4x4& a)
        {
            return { a.a1, a.b1, a.c1, a.d1, a.a2, a.b2, a.c2, a.d2,
                     a.a3, a.b3, a.c3, a.d3, a.a4, a.b4, a.c4, a.d4 };
        }
        void checkMatrix(const glm::mat4& value)
        {
            for (unsigned column{}; column < 4; ++column)
                for (unsigned row{}; row < 4; ++row)
                    if (!std::isfinite(value[column][row]))
                        throw std::runtime_error{ "non-finite model transform" };
            if (value[0][3] != 0 || value[1][3] != 0 || value[2][3] != 0 || value[3][3] != 1 ||
                std::abs(glm::determinant(glm::mat3{ value })) < 1e-15F)
                throw std::runtime_error{ "model transform must be affine and nonsingular" };
        }
        ModelAsset copyScene(const aiScene* scene,
                             const Assimp::Importer& importer,
                             ModelCoordinates coordinates)
        {
            if (!scene || !scene->mRootNode || !scene->HasMeshes())
                throw std::runtime_error{ std::string{ "Assimp: " } + importer.GetErrorString() };
            if (scene->mNumMeshes > 4096 || scene->mNumMaterials > 4096)
                throw std::length_error{ "model mesh/material capacity exceeded" };
            if (scene->HasAnimations())
                throw std::runtime_error{ "robot parts must be rigid exports without animations" };
            ModelAsset result{};
            std::size_t vertex_count{}, index_count{};
            for (unsigned i{}; i < scene->mNumMeshes; ++i) {
                const auto& mesh{ *scene->mMeshes[i] };
                vertex_count += mesh.mNumVertices;
                index_count += static_cast<std::size_t>(mesh.mNumFaces) * 3;
                if (vertex_count > MAX_VERTICES || index_count > MAX_INDICES)
                    throw std::length_error{ "model geometry capacity exceeded" };
                if (!mesh.HasPositions() || !mesh.HasNormals() || mesh.HasBones() || mesh.mNumAnimMeshes)
                    throw std::runtime_error{ "model requires rigid triangle geometry with valid normals" };
                ModelGeometry geometry{};
                geometry.vertices.reserve(mesh.mNumVertices);
                geometry.indices.reserve(static_cast<std::size_t>(mesh.mNumFaces) * 3);
                for (unsigned v{}; v < mesh.mNumVertices; ++v) {
                    const auto& position{ mesh.mVertices[v] };
                    const auto& normal{ mesh.mNormals[v] };
                    Vertex vertex{ { position.x, position.y, position.z }, { normal.x, normal.y, normal.z } };
                    if (!finite(vertex.position) || !finite(vertex.normal) ||
                        !std::isfinite(glm::dot(vertex.normal, vertex.normal)) ||
                        glm::dot(vertex.normal, vertex.normal) < 1e-12F)
                        throw std::runtime_error{ "invalid model vertex or normal" };
                    vertex.normal = glm::normalize(vertex.normal);
                    geometry.vertices.push_back(vertex);
                }
                for (unsigned f{}; f < mesh.mNumFaces; ++f) {
                    const auto& face{ mesh.mFaces[f] };
                    if (face.mNumIndices != 3)
                        throw std::runtime_error{ "model contains non-triangle geometry" };
                    for (unsigned c{}; c < 3; ++c) {
                        if (face.mIndices[c] >= mesh.mNumVertices)
                            throw std::runtime_error{ "invalid model index" };
                        geometry.indices.push_back(face.mIndices[c]);
                    }
                }
                if (geometry.indices.empty())
                    throw std::runtime_error{ "empty model mesh" };
                if (mesh.mMaterialIndex >= scene->mNumMaterials)
                    throw std::runtime_error{ "invalid model material index" };
                const auto& material{ *scene->mMaterials[mesh.mMaterialIndex] };
                aiColor4D color{ .7F, .7F, .7F, 1 };
                if (material.Get(AI_MATKEY_BASE_COLOR, color) != AI_SUCCESS)
                    material.Get(AI_MATKEY_COLOR_DIFFUSE, color);
                float opacity{ 1 };
                material.Get(AI_MATKEY_OPACITY, opacity);
                // Importers often duplicate base-color alpha in opacity. Do not square it.
                geometry.color = { color.r, color.g, color.b, std::min(color.a, opacity) };
                for (unsigned c{}; c < 4; ++c)
                    if (!std::isfinite(geometry.color[c]) || geometry.color[c] < 0 || geometry.color[c] > 1)
                        throw std::runtime_error{ "invalid model material color" };
                if (material.GetTextureCount(aiTextureType_BASE_COLOR) ||
                    material.GetTextureCount(aiTextureType_DIFFUSE) ||
                    material.GetTextureCount(aiTextureType_NORMALS))
                    result.warnings.emplace_back("Texture maps are not rendered; using CAD material colors.");
                result.geometries.push_back(std::move(geometry));
            }
            glm::vec3 minimum{ std::numeric_limits<float>::max() }, maximum{ -minimum };
            std::size_t nodes{}, instanced_vertices{};
            const std::function<void(const aiNode&, const glm::mat4&, unsigned)> visit{
                [&](const aiNode& node, const glm::mat4& parent, unsigned depth) {
                if (++nodes > MAX_NODES || depth > 128)
                    throw std::length_error{ "model hierarchy capacity exceeded" };
                const auto transform{ parent * matrix(node.mTransformation) };
                checkMatrix(transform);
                for (unsigned i{}; i < node.mNumMeshes; ++i) {
                    if (node.mMeshes[i] >= result.geometries.size() || result.instances.size() >= MAX_NODES)
                        throw std::runtime_error{ "invalid model instance" };
                    const auto& geometry{ result.geometries[node.mMeshes[i]] };
                    instanced_vertices += geometry.vertices.size();
                    if (instanced_vertices > 10'000'000)
                        throw std::length_error{ "instanced model vertex capacity exceeded" };
                    glm::vec3 lo{ std::numeric_limits<float>::max() }, hi{ -lo };
                    for (const auto& vertex : geometry.vertices) {
                        const glm::vec3 position{ transform * glm::vec4{ vertex.position, 1 } };
                        if (!finite(position) ||
                            glm::any(glm::greaterThan(glm::abs(position), glm::vec3{ 10000 })))
                            throw std::runtime_error{
                                "model exceeds 10 m bounds; check export units/origin"
                            };
                        lo = glm::min(lo, position);
                        hi = glm::max(hi, position);
                    }
                    minimum = glm::min(minimum, lo);
                    maximum = glm::max(maximum, hi);
                    result.instances.push_back({ node.mMeshes[i], transform, (lo + hi) * .5F });
                }
                for (unsigned i{}; i < node.mNumChildren; ++i)
                    visit(*node.mChildren[i], transform, depth + 1);
            }
            };
            visit(*scene->mRootNode, coordinates.matrix(), 0);
            if (result.instances.empty() || glm::length(maximum - minimum) < .001F)
                throw std::runtime_error{ "model has no visible geometry" };
            result.minimum = minimum;
            result.maximum = maximum;
            std::sort(result.warnings.begin(), result.warnings.end());
            result.warnings.erase(std::unique(result.warnings.begin(), result.warnings.end()),
                                  result.warnings.end());
            return result;
        }
    }
    glm::mat4 ModelCoordinates::matrix() const
    {
        if (!std::isfinite(millimetres_per_unit) || millimetres_per_unit < .001 ||
            millimetres_per_unit > 1000000)
            throw std::invalid_argument{ "model scale must be finite and within .001..1000000 mm/unit" };
        auto result{ glm::scale(glm::mat4{ 1 }, glm::vec3{ static_cast<float>(millimetres_per_unit) }) };
        if (up == UpAxis::Y)
            result = glm::rotate(result, glm::radians(90.0F), glm::vec3{ 1, 0, 0 });
        return result;
    }
    ModelAsset ModelAsset::read(const std::filesystem::path& file, ModelCoordinates coordinates)
    {
        coordinates.matrix();
        if (std::filesystem::file_size(file) > 256 * 1024 * 1024)
            throw std::length_error{ "model file exceeds 256 MiB" };
        Assimp::Importer importer{};
        const auto* scene{ importer.ReadFile(file.string(), FLAGS) };
        try {
            return copyScene(scene, importer, coordinates);
        } catch (const std::exception& e) {
            throw std::runtime_error{ file.string() + ": " + e.what() };
        }
    }
    ModelAsset ModelAsset::read(std::span<const std::byte> bytes,
                                const char* format,
                                ModelCoordinates coordinates)
    {
        coordinates.matrix();
        if (bytes.empty() || bytes.size() > 256 * 1024 * 1024)
            throw std::length_error{ "model data is empty or exceeds 256 MiB" };
        Assimp::Importer importer{};
        const auto* scene{ importer.ReadFileFromMemory(bytes.data(), bytes.size(), FLAGS, format) };
        return copyScene(scene, importer, coordinates);
    }
    RobotModels RobotModels::load(const std::filesystem::path& directory, ModelCoordinates coordinates)
    {
        RobotModels result{};
        if (directory.empty()) {
            const auto embedded{ assets::robotModels() };
            for (std::size_t i{}; i < result.parts.size(); ++i)
                result.parts[i] = ModelAsset::read(embedded[i], "dae");
        }
        else {
            // Exact visual filenames: never select the neighboring collision STLs.
            constexpr std::array names{ "base_link", "link_1", "link_2", "link_3" };
            for (std::size_t i{}; i < names.size(); ++i)
                result.parts[i] =
                  ModelAsset::read(directory / names[i] / (std::string{ names[i] } + ".dae"), coordinates);
            result.external = true;
        }
        return result;
    }
    std::array<glm::mat4, 4> robotModelTransforms(const Profile& profile, const std::array<double, 3>& joints)
    {
        for (double value : joints)
            if (!std::isfinite(value))
                throw std::invalid_argument{ "invalid rendered joint state" };
        for (double length : profile.geometry_mm)
            if (!std::isfinite(length) || length <= 0 || length > 10000)
                throw std::invalid_argument{ "invalid rendered robot geometry" };
        if (std::abs(joints[2]) > 10000)
            throw std::invalid_argument{ "rendered Z exceeds 10 m" };
        const auto first{ glm::rotate(
          glm::translate(glm::mat4{ 1 }, glm::vec3{ 0, 0, static_cast<float>(cad::J1_FRAME_Z_MM) }),
          static_cast<float>(std::remainder(joints[0], 2 * std::numbers::pi)),
          glm::vec3{ 0, 0, 1 }) };
        const auto second{ glm::rotate(
          glm::translate(first,
                         glm::vec3{ static_cast<float>(profile.geometry_mm[0]),
                                    0,
                                    static_cast<float>(cad::J2_FRAME_Z_MM - cad::J1_FRAME_Z_MM) }),
          static_cast<float>(std::remainder(joints[1], 2 * std::numbers::pi)),
          glm::vec3{ 0, 0, 1 }) };
        const auto third{ glm::translate(
          second,
          glm::vec3{ static_cast<float>(profile.geometry_mm[1] + profile.geometry_mm[2]),
                     0,
                     static_cast<float>(joints[2] - cad::TOOL_TIP_LOCAL_Z_MM - cad::J2_FRAME_Z_MM) }) };
        return { glm::mat4{ 1 }, first, second, third };
    }
}
