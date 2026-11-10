#include "ModelAsset.hpp"
#include "../Kinematics.hpp"
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
        constexpr unsigned MAX_MESHES{ 4096 }, MAX_HIERARCHY_DEPTH{ 128 };
        constexpr std::size_t MAX_INSTANCED_VERTICES{ 10'000'000 }, MAX_ASSET_BYTES{ 256UZ * 1024 * 1024 };
        constexpr float MATRIX_EPS{ 1e-15F }, NORMAL_EPS{ 1e-12F }, DEFAULT_GRAY{ .7F },
          MAX_EXTENT_MM{ 10000 }, MIN_EXTENT_MM{ .001F }, MIDPOINT{ .5F };
        constexpr double MIN_SCALE{ .001 }, MAX_SCALE{ 1000000 };
        constexpr float Y_UP_ROTATION{ 90.F };
        constexpr unsigned FLAGS{ aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                  aiProcess_GenSmoothNormals | aiProcess_ValidateDataStructure |
                                  aiProcess_ImproveCacheLocality };
        bool finite(glm::vec3 value)
        {
            return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
        }
        glm::mat4 matrix(const aiMatrix4x4& a)
        {
            return { a.a1, a.b1, a.c1, a.d1, a.a2, a.b2, a.c2, a.d2,
                     a.a3, a.b3, a.c3, a.d3, a.a4, a.b4, a.c4, a.d4 };
        }
        void check_matrix(const glm::mat4& value)
        {
            for (glm::length_t column{}; column < 4; ++column) {
                for (glm::length_t row{}; row < 4; ++row) {
                    if (!std::isfinite(value[column][row])) {
                        throw std::runtime_error{ "non-finite model transform" };
                    }
                }
            }
            if (value[0][3] != 0 || value[1][3] != 0 || value[2][3] != 0 || value[3][3] != 1 ||
                std::abs(glm::determinant(glm::mat3{ value })) < MATRIX_EPS) {
                throw std::runtime_error{ "model transform must be affine and nonsingular" };
            }
        }
        ModelGeometry copy_geometry(const aiMesh& mesh)
        {
            if (!mesh.HasPositions() || !mesh.HasNormals() || mesh.HasBones() || mesh.mNumAnimMeshes) {
                throw std::runtime_error{ "model requires rigid triangle geometry with valid normals" };
            }
            ModelGeometry geometry{};
            geometry.vertices.reserve(mesh.mNumVertices);
            geometry.indices.reserve(static_cast<std::size_t>(mesh.mNumFaces) * 3);
            for (unsigned v{}; v < mesh.mNumVertices; ++v) {
                const auto& position{ mesh.mVertices[v] };
                const auto& normal{ mesh.mNormals[v] };
                Vertex vertex{ .position = { position.x, position.y, position.z },
                               .normal = { normal.x, normal.y, normal.z } };
                if (!finite(vertex.position) || !finite(vertex.normal) ||
                    !std::isfinite(glm::dot(vertex.normal, vertex.normal)) ||
                    glm::dot(vertex.normal, vertex.normal) < NORMAL_EPS) {
                    throw std::runtime_error{ "invalid model vertex or normal" };
                }
                vertex.normal = glm::normalize(vertex.normal);
                geometry.vertices.push_back(vertex);
            }
            for (unsigned f{}; f < mesh.mNumFaces; ++f) {
                const auto& face{ mesh.mFaces[f] };
                if (face.mNumIndices != 3) {
                    throw std::runtime_error{ "model contains non-triangle geometry" };
                }
                for (glm::length_t c{}; c < 3; ++c) {
                    if (face.mIndices[c] >= mesh.mNumVertices) {
                        throw std::runtime_error{ "invalid model index" };
                    }
                    geometry.indices.push_back(face.mIndices[c]);
                }
            }
            if (geometry.indices.empty()) {
                throw std::runtime_error{ "empty model mesh" };
            }
            return geometry;
        }
        void copy_material(ModelGeometry& geometry,
                           const aiMaterial& material,
                           std::vector<std::string>& warnings)
        {
            aiColor4D color{ DEFAULT_GRAY, DEFAULT_GRAY, DEFAULT_GRAY, 1 };
            if (material.Get(AI_MATKEY_BASE_COLOR, color) != AI_SUCCESS) {
                material.Get(AI_MATKEY_COLOR_DIFFUSE, color);
            }
            float opacity{ 1 };
            material.Get(AI_MATKEY_OPACITY, opacity);
            // Importers often duplicate base-color alpha in opacity. Do not square it.
            geometry.color = { color.r, color.g, color.b, std::min(color.a, opacity) };
            for (glm::length_t c{}; c < 4; ++c) {
                if (!std::isfinite(geometry.color[c]) || geometry.color[c] < 0 || geometry.color[c] > 1) {
                    throw std::runtime_error{ "invalid model material color" };
                }
            }
            if (material.GetTextureCount(aiTextureType_BASE_COLOR) ||
                material.GetTextureCount(aiTextureType_DIFFUSE) ||
                material.GetTextureCount(aiTextureType_NORMALS)) {
                warnings.emplace_back("Texture maps are not rendered; using CAD material colors.");
            }
        }
        struct Hierarchy
        {
            std::reference_wrapper<ModelAsset> result;
            glm::vec3 minimum{ std::numeric_limits<float>::max() };
            glm::vec3 maximum{ -minimum };
            std::size_t nodes{}, instanced_vertices{};
            void visit(const aiNode& node, const glm::mat4& parent, unsigned depth)
            {
                if (++nodes > MAX_NODES || depth > MAX_HIERARCHY_DEPTH) {
                    throw std::length_error{ "model hierarchy capacity exceeded" };
                }
                const auto transform{ parent * matrix(node.mTransformation) };
                check_matrix(transform);
                for (unsigned i{}; i < node.mNumMeshes; ++i) {
                    if (node.mMeshes[i] >= result.get().geometries.size() ||
                        result.get().instances.size() >= MAX_NODES) {
                        throw std::runtime_error{ "invalid model instance" };
                    }
                    const auto& geometry{ result.get().geometries[node.mMeshes[i]] };
                    instanced_vertices += geometry.vertices.size();
                    if (instanced_vertices > MAX_INSTANCED_VERTICES) {
                        throw std::length_error{ "instanced model vertex capacity exceeded" };
                    }
                    glm::vec3 lo{ std::numeric_limits<float>::max() };
                    glm::vec3 hi{ -lo };
                    for (const auto& vertex : geometry.vertices) {
                        const glm::vec3 position{ transform * glm::vec4{ vertex.position, 1 } };
                        if (!finite(position) ||
                            glm::any(glm::greaterThan(glm::abs(position), glm::vec3{ MAX_EXTENT_MM }))) {
                            throw std::runtime_error{
                                "model exceeds 10 m bounds; check export units/origin"
                            };
                        }
                        lo = glm::min(lo, position);
                        hi = glm::max(hi, position);
                    }
                    minimum = glm::min(minimum, lo);
                    maximum = glm::max(maximum, hi);
                    result.get().instances.push_back({ node.mMeshes[i], transform, (lo + hi) * MIDPOINT });
                }
                for (unsigned i{}; i < node.mNumChildren; ++i) {
                    visit(*node.mChildren[i], transform, depth + 1);
                }
            }
        };
        ModelAsset copy_scene(const aiScene* scene,
                              const Assimp::Importer& importer,
                              ModelCoordinates coordinates)
        {
            if (!scene || !scene->mRootNode || !scene->HasMeshes()) {
                throw std::runtime_error{ std::string{ "Assimp: " } + importer.GetErrorString() };
            }
            if (scene->mNumMeshes > MAX_MESHES || scene->mNumMaterials > MAX_MESHES) {
                throw std::length_error{ "model mesh/material capacity exceeded" };
            }
            if (scene->HasAnimations()) {
                throw std::runtime_error{ "robot parts must be rigid exports without animations" };
            }
            ModelAsset result{};
            std::size_t vertex_count{};
            std::size_t index_count{};
            for (unsigned i{}; i < scene->mNumMeshes; ++i) {
                const auto& mesh{ *scene->mMeshes[i] };
                vertex_count += mesh.mNumVertices;
                index_count += static_cast<std::size_t>(mesh.mNumFaces) * 3;
                if (vertex_count > MAX_VERTICES || index_count > MAX_INDICES) {
                    throw std::length_error{ "model geometry capacity exceeded" };
                }
                auto geometry{ copy_geometry(mesh) };
                if (mesh.mMaterialIndex >= scene->mNumMaterials) {
                    throw std::runtime_error{ "invalid model material index" };
                }
                const auto& material{ *scene->mMaterials[mesh.mMaterialIndex] };
                copy_material(geometry, material, result.warnings);
                result.geometries.push_back(std::move(geometry));
            }
            Hierarchy hierarchy{ .result = result };
            hierarchy.visit(*scene->mRootNode, coordinates.matrix(), 0);
            const auto minimum{ hierarchy.minimum };
            const auto maximum{ hierarchy.maximum };
            if (result.instances.empty() || glm::length(maximum - minimum) < MIN_EXTENT_MM) {
                throw std::runtime_error{ "model has no visible geometry" };
            }
            result.minimum = minimum;
            result.maximum = maximum;
            std::ranges::sort(result.warnings);
            result.warnings.erase(std::ranges::unique(result.warnings).begin(), result.warnings.end());
            return result;
        }
    }
    glm::mat4 ModelCoordinates::matrix() const
    {
        if (!std::isfinite(millimetres_per_unit) || millimetres_per_unit < MIN_SCALE ||
            millimetres_per_unit > MAX_SCALE) {
            throw std::invalid_argument{ "model scale must be finite and within .001..1000000 mm/unit" };
        }
        auto result{ glm::scale(glm::mat4{ 1 }, glm::vec3{ static_cast<float>(millimetres_per_unit) }) };
        if (up == UpAxis::Y) {
            result = glm::rotate(result, glm::radians(Y_UP_ROTATION), glm::vec3{ 1, 0, 0 });
        }
        return result;
    }
    ModelAsset ModelAsset::read(const std::filesystem::path& file, ModelCoordinates coordinates)
    {
        coordinates.matrix();
        if (std::filesystem::file_size(file) > MAX_ASSET_BYTES) {
            throw std::length_error{ "model file exceeds 256 MiB" };
        }
        Assimp::Importer importer{};
        const auto* scene{ importer.ReadFile(file.string(), FLAGS) };
        try {
            return copy_scene(scene, importer, coordinates);
        } catch (const std::exception& e) {
            throw std::runtime_error{ file.string() + ": " + e.what() };
        }
    }
    ModelAsset ModelAsset::read(std::span<const std::byte> bytes,
                                const char* format,
                                ModelCoordinates coordinates)
    {
        coordinates.matrix();
        if (bytes.empty() || bytes.size() > MAX_ASSET_BYTES) {
            throw std::length_error{ "model data is empty or exceeds 256 MiB" };
        }
        Assimp::Importer importer{};
        const auto* scene{ importer.ReadFileFromMemory(bytes.data(), bytes.size(), FLAGS, format) };
        return copy_scene(scene, importer, coordinates);
    }
    RobotModels RobotModels::load(const std::filesystem::path& directory, ModelCoordinates coordinates)
    {
        RobotModels result{};
        if (directory.empty()) {
            const auto embedded{ assets::robot_models() };
            for (std::size_t i{}; i < result.parts.size(); ++i) {
                result.parts[i] = ModelAsset::read(embedded[i], "dae");
            }
        }
        else {
            // Exact visual filenames: never select the neighboring collision STLs.
            constexpr std::array names{ "base_link", "link_1", "link_2", "link_3" };
            for (std::size_t i{}; i < names.size(); ++i) {
                result.parts[i] =
                  ModelAsset::read(directory / names[i] / (std::string{ names[i] } + ".dae"), coordinates);
            }
            result.external = true;
        }
        return result;
    }
    std::array<glm::mat4, 4> robot_model_transforms(const Profile& profile,
                                                    const std::array<double, 3>& joints)
    {
        for (double value : joints) {
            if (!std::isfinite(value)) {
                throw std::invalid_argument{ "invalid rendered joint state" };
            }
        }
        for (double length : profile.geometry_mm) {
            if (!std::isfinite(length) || length <= 0 || length > MAX_EXTENT_MM) {
                throw std::invalid_argument{ "invalid rendered robot geometry" };
            }
        }
        if (std::abs(joints[2]) > MAX_EXTENT_MM) {
            throw std::invalid_argument{ "rendered Z exceeds 10 m" };
        }
        const auto frames{ Kinematics::frames(profile, joints) };
        return {
            glm::mat4{ frames[0] }, glm::mat4{ frames[1] }, glm::mat4{ frames[2] }, glm::mat4{ frames[3] }
        };
    }
}
