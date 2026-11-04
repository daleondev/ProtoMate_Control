#include "../assets/EmbeddedAssets.hpp"
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
    require(!models.external, "default geometry is embedded Fusion CAD");
    // Independently measured export bounds catch accidental double unit/up conversion.
    constexpr std::array<glm::vec3, 4> minimum{ { { -157.424622F, -115.000000F, 0.000000F },
                                                  { -70.000000F, -71.145163F, 2.000000F },
                                                  { -40.000000F, -47.049999F, -308.999992F },
                                                  { -51.000004F, -44.000001F, -280.000000F } } };
    constexpr std::array<glm::vec3, 4> maximum{ { { 105.000000F, 115.000000F, 198.100000F },
                                                  { 244.995499F, 71.145163F, 295.000000F },
                                                  { 335.399998F, 73.500000F, 47.000002F },
                                                  { 24.977570F, 47.700000F, -25.000000F } } };
    constexpr std::array<std::size_t, 4> bodies{ 143, 60, 146, 23 };
    for (std::size_t i{}; i < models.parts.size(); ++i) {
        const auto& part{ models.parts[i] };
        require(near(part.minimum, minimum[i]) && near(part.maximum, maximum[i]),
                "CAD link bounds in mm/Z-up");
        require(part.warnings.empty(), "embedded models must not drop texture data");
        require(part.instances.size() >= bodies[i], "every exported CAD body loaded");
        bool colored{};
        for (const auto& geometry : part.geometries) {
            colored |= std::abs(geometry.color.r - geometry.color.b) > .1F;
            for (const auto& vertex : geometry.vertices)
                require(std::abs(glm::length(vertex.normal) - 1) < .001F, "unit normals");
        }
        require(colored, "CAD material colors preserved");
    }
    const Profile profile{};
    for (const auto q : std::array<std::array<double, 3>, 4>{
           { { 0, 0, 0 }, { .4, -.8, 73 }, { 7, -9, -15 }, { -12, 14, 190 } } }) {
        const auto transforms{ robotModelTransforms(profile, q) };
        const float a{ static_cast<float>(q[0]) }, b{ static_cast<float>(q[0] + q[1]) };
        const glm::vec3 elbow{ 205 * std::cos(a), 205 * std::sin(a), 369.9F };
        require(near(glm::vec3{ transforms[0] * glm::vec4{ 10, 20, 30, 1 } }, { 10, 20, 30 }),
                "base remains fixed");
        require(near(glm::vec3{ transforms[1] * glm::vec4{ 0, 0, 0, 1 } }, { 0, 0, 52 }),
                "STEP shoulder export datum");
        require(near(glm::vec3{ transforms[2] * glm::vec4{ 0, 0, 0, 1 } }, elbow), "elbow follows shoulder");
        const glm::vec3 expected{ 205 * std::cos(a) + 358.4F * std::cos(b),
                                  205 * std::sin(a) + 358.4F * std::sin(b),
                                  static_cast<float>(q[2] + 28.9) };
        require(near(glm::vec3{ transforms[3] * glm::vec4{ 0, 0, -280, 1 } }, expected),
                "actual exported cutter tip matches FK for all joints/multiple turns");
    }
    const auto rest{ robotModelTransforms(profile, { 0, 0, 0 }) };
    require(near(glm::vec3{ rest[3] * glm::vec4{ 0, 0, 0, 1 } }, { 563.4F, 0, 308.9F }),
            "export rest pose reproduces STEP carriage datum");
    // Independent STEP plunger centers: each sensor follows its proper parent link.
    constexpr std::array<glm::vec3, 3> sensors{
        { { -78.7F, .4F, 195.2F }, { 154.1F, .4F, 273.8F }, { 526.6F, 49.7F, 263.4F } }
    };
    for (std::size_t i{}; i < sensors.size(); ++i) {
        const auto& mount{ cad::HOME_SENSOR_MOUNTS[i] };
        const auto& c{ mount.plunger_center_mm };
        require(
          near(glm::vec3{ rest[static_cast<unsigned>(mount.parent)] * glm::vec4{ c[0], c[1], c[2], 1 } },
               sensors[i]),
          "sensor mounting coordinates");
    }
    static constexpr unsigned char instances_dae[]{
#embed "models/instances.dae"
    };
    const auto instanced{ ModelAsset::read(
      std::as_bytes(std::span{ instances_dae }), "dae", { 1, UpAxis::Z }) };
    require(instanced.instances.size() == 2 && instanced.geometries.size() == 1 &&
              instanced.instances[0].geometry == instanced.instances[1].geometry,
            "repeated scene nodes share uploaded mesh geometry");
    require(near(instanced.minimum, { 10, 0, 0 }) && near(instanced.maximum, { 12, 22, 0 }),
            "nested node transforms accumulate in parent-child order");
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
    const auto embedded{ assets::robotModels() };
    constexpr std::array names{ "base_link", "link_1", "link_2", "link_3" };
    for (std::size_t i{}; i < names.size(); ++i) {
        const auto dir{ std::filesystem::path{ directory.name.data() } / names[i] };
        std::filesystem::create_directory(dir);
        std::ofstream file{ dir / (std::string{ names[i] } + ".dae"), std::ios::binary };
        file.write(reinterpret_cast<const char*>(embedded[i].data()),
                   static_cast<std::streamsize>(embedded[i].size()));
        std::ofstream{ dir / (std::string{ names[i] } + "_collision.stl") }
          << "unused, intentionally invalid";
    }
    const auto external{ RobotModels::load(directory.name.data()) };
    require(external.external, "explicit external source");
    for (std::size_t i{}; i < names.size(); ++i)
        require(near(external.parts[i].minimum, models.parts[i].minimum) &&
                  near(external.parts[i].maximum, models.parts[i].maximum),
                "external/embedded DAE parity; collision STLs ignored");
    {
        std::ofstream material{ std::filesystem::path{ directory.name.data() } / "colors.mtl" };
        material << "newmtl tint\nKd 0.2 0.4 0.8\nd 0.5\n";
        std::ofstream mesh{ std::filesystem::path{ directory.name.data() } / "material.obj" };
        mesh << "mtllib colors.mtl\nusemtl tint\n" << OBJ;
    }
    const auto material{ ModelAsset::read(std::filesystem::path{ directory.name.data() } / "material.obj",
                                          { 1, UpAxis::Z }) };
    const auto tint{ material.geometries.front().color };
    require(near(glm::vec3{ tint }, { .2F, .4F, .8F }) && std::abs(tint.a - .5F) < .001F,
            "OBJ sidecar material color and alpha preserved, not squared");
    std::puts("PASS: four Fusion DAE links, materials, units, STEP registration, articulated TCP/sensors, "
              "external overrides, ignored collision STLs and malformed input");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
