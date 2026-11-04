#include "../viewer/ModelAsset.hpp"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

using namespace simulation;
using namespace simulation::viewer;
namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error{ message };
    }
    template<typename F>
    void rejects(F&& operation, const char* message)
    {
        bool rejected{};
        try {
            operation();
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, message);
    }
    bool near(glm::vec3 a, glm::vec3 b) { return glm::length(a - b) < .002F; }
    struct Directory
    {
        std::array<char, 40> name{};
        Directory()
        {
            std::snprintf(name.data(), name.size(), "/tmp/protomate-model-test-XXXXXX");
            if (!mkdtemp(name.data()))
                throw std::runtime_error{ "temporary model directory" };
        }
        ~Directory()
        {
            std::error_code error{};
            std::filesystem::remove_all(name.data(), error);
        }
    };
    constexpr std::string_view OBJ{ "o triangle\nv 0 0 0\nv 10 0 0\nv 0 20 0\nf 1 2 3\n" };
}
int main()
try {
    const auto models{ RobotModels::load() };
    require(models.placeholders, "default geometry must be explicitly synthetic");
    require(near(models.parts[0].minimum, { -40, -40, -28 }),
            "GLB hierarchy/rotation/unit conversion minimum");
    require(near(models.parts[0].maximum, { 237, 40, 22 }), "GLB hierarchy/rotation/unit conversion maximum");
    bool instanced{}, blue{};
    for (const auto& part : models.parts) {
        require(part.warnings.empty(), "embedded models must not drop texture data");
        require(!part.instances.empty(), "all three dummy models loaded");
        for (const auto& a : part.instances)
            for (const auto& b : part.instances)
                if (&a != &b && a.geometry == b.geometry)
                    instanced = true;
        for (const auto& geometry : part.geometries) {
            blue |= near(glm::vec3{ geometry.color }, { .16F, .43F, .64F });
            for (const auto& v : geometry.vertices)
                require(std::abs(glm::length(v.normal) - 1) < .001F, "unit normals");
        }
    }
    require(instanced, "repeated CAD instances must share geometry");
    require(blue, "imported CAD material colors");
    const Profile profile{};
    for (const auto q : std::array<std::array<double, 3>, 4>{
           { { 0, 0, 0 }, { .4, -.8, 73 }, { 7, -9, -15 }, { -12, 14, 190 } } }) {
        const auto transforms{ robotModelTransforms(profile, q) };
        const float a{ static_cast<float>(q[0]) }, b{ static_cast<float>(q[0] + q[1]) };
        const glm::vec3 elbow{ 205 * std::cos(a), 205 * std::sin(a), 140 };
        require(near(glm::vec3{ transforms[0] * glm::vec4{ 0, 0, 0, 1 } }, { 0, 0, 140 }),
                "fixed shoulder pivot");
        require(near(glm::vec3{ transforms[1] * glm::vec4{ 0, 0, 0, 1 } }, elbow), "elbow follows shoulder");
        const glm::vec3 expected{ 205 * std::cos(a) + 358.4F * std::cos(b),
                                  205 * std::sin(a) + 358.4F * std::sin(b),
                                  static_cast<float>(q[2]) };
        require(near(glm::vec3{ transforms[2] * glm::vec4{ 135, 0, 0, 1 } }, expected),
                "rendered TCP matches FK");
        const auto higher{ robotModelTransforms(profile, q, 230) };
        require(near(glm::vec3{ higher[2] * glm::vec4{ 135, 0, 0, 1 } }, expected),
                "mount height never changes TCP");
    }
    const auto bytes{ std::as_bytes(std::span{ OBJ.data(), OBJ.size() }) };
    const auto obj{ ModelAsset::read(bytes, "obj", { 1, UpAxis::Z }) };
    require(near(obj.maximum, { 10, 20, 0 }), "millimetre Z-up OBJ is not rescaled implicitly");
    const auto rotated{ ModelAsset::read(bytes, "obj", { 10, UpAxis::Y }) };
    require(near(rotated.maximum, { 100, 0, 200 }), "explicit unit and up-axis conversion");
    rejects([&] { ModelAsset::read({}, "glb"); }, "empty model rejection");
    rejects([&] { ModelAsset::read(bytes, "obj", { 0, UpAxis::Z }); }, "zero scale rejection");
    rejects([&] { ModelAsset::read(bytes, "obj", { std::numeric_limits<double>::infinity(), UpAxis::Z }); },
            "infinite scale rejection");
    rejects([&] { ModelAsset::read(bytes, "obj", { 100000, UpAxis::Z }); },
            "implausible model size rejection");
    const std::string_view corrupt{ "not a model" };
    rejects([&] { ModelAsset::read(std::as_bytes(std::span{ corrupt.data(), corrupt.size() }), "glb"); },
            "corrupt model rejection");
    rejects([&] { robotModelTransforms(profile, { 0, std::numeric_limits<double>::quiet_NaN(), 0 }); },
            "invalid pose rejection");
    Directory directory{};
    rejects([&] { RobotModels::load(directory.name.data()); }, "no fallback for missing requested CAD files");
    for (const auto name : { "shoulder", "forearm", "carriage" }) {
        std::ofstream file{ std::filesystem::path{ directory.name.data() } / (std::string{ name } + ".obj") };
        file << OBJ;
    }
    {
        std::ofstream material{ std::filesystem::path{ directory.name.data() } / "colors.mtl" };
        material << "newmtl tint\nKd 0.2 0.4 0.8\nd 0.5\n";
        std::ofstream mesh{ std::filesystem::path{ directory.name.data() } / "forearm.obj" };
        mesh << "mtllib colors.mtl\nusemtl tint\n" << OBJ;
    }
    const auto external{ RobotModels::load(directory.name.data(), { 1, UpAxis::Z }) };
    require(!external.placeholders && near(external.parts[2].maximum, { 10, 20, 0 }), "external override");
    const auto tint{ external.parts[1].geometries.front().color };
    require(near(glm::vec3{ tint }, { .2F, .4F, .8F }) && std::abs(tint.a - .5F) < .001F,
            "OBJ sidecar material color and alpha must be preserved, not squared");
    std::ofstream{ std::filesystem::path{ directory.name.data() } / "shoulder.stl" } << "invalid";
    rejects([&] { RobotModels::load(directory.name.data()); }, "ambiguous overrides rejected");
    std::puts("PASS: Assimp imports, node hierarchy, materials, instancing, units, all joint pivots/FK, "
              "external overrides and malformed input");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
