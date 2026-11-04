#include "../viewer/Gl.hpp"
#include "../viewer/ModelRenderer.hpp"
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>
using namespace simulation::viewer;
namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error{ message };
    }
}
int main()
try {
    SdlSession sdl;
    Window window;
    Context context{ window };
    static constexpr char vertex[]{
#embed "shaders/test.vert"
    };
    static constexpr char fragment[]{
#embed "shaders/test.frag"
    };
    static constexpr char invalid_source[]{
#embed "shaders/invalid.vert"
    };
    bool failed{};
    try {
        Shader invalid{ context, GL_VERTEX_SHADER, { invalid_source, sizeof(invalid_source) } };
    } catch (const std::runtime_error& e) {
        failed = std::string_view{ e.what() }.size() > 10;
    }
    require(failed, "shader errors must include compiler log");
    Program original{ context, { vertex, sizeof(vertex) }, { fragment, sizeof(fragment) } };
    const auto id{ original.get() };
    Program program{ std::move(original) };
    require(program.get() == id, "program move ownership");
    Buffer buffer{ context };
    const auto buffer_id{ buffer.get() };
    {
        Buffer moved{ std::move(buffer) };
        require(glIsBuffer(moved.get()), "buffer moved alive");
    }
    require(!glIsBuffer(buffer_id), "buffer destruction releases object");
    RenderTarget target{ context, 32, 32 };
    target.resize(context, 64, 64);
    bool dimensions{};
    try {
        target.resize(context, 0, 64);
    } catch (const std::invalid_argument&) {
        dimensions = true;
    }
    require(dimensions, "invalid resize rejected");
    unsigned rejected_accesses{};
    std::thread wrong_thread{ [&] {
        try {
            program.get();
        } catch (const std::logic_error&) {
            ++rejected_accesses;
        }
        try {
            target.get();
        } catch (const std::logic_error&) {
            ++rejected_accesses;
        }
    } };
    wrong_thread.join();
    require(rejected_accesses == 2, "cross-thread GL access must throw, not terminate or return a handle");
    std::array<Vertex, 3> vertices{ { { { -1, -1, 0 }, {} }, { { 1, -1, 0 }, {} }, { { 0, 1, 0 }, {} } } };
    Mesh mesh{ context, vertices };
    glBindFramebuffer(GL_FRAMEBUFFER, target.get());
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(program.get());
    mesh.draw();
    std::array<unsigned char, 4> pixel{};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    require(pixel[0] >= 50 && pixel[0] <= 52 && pixel[1] >= 203 && pixel[1] <= 205 && pixel[2] >= 101 &&
              pixel[2] <= 103,
            "actual GLSL draw/readback");
    const auto models{ RobotModels::load() };
    ModelRenderer robot{ context, models };
    target.resize(context, 256, 256);
    glBindFramebuffer(GL_FRAMEBUFFER, target.get());
    glViewport(0, 0, 256, 256);
    glEnable(GL_DEPTH_TEST);
    const glm::vec3 eye{ 650, -650, 500 };
    const auto vp{ glm::perspective(glm::radians(45.0F), 1.0F, 1.0F, 3000.0F) *
                   glm::lookAt(eye, glm::vec3{ 200, 0, 200 }, glm::vec3{ 0, 0, 1 }) };
    const auto capture{ [&](std::array<double, 3> q) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        robot.draw(robotModelTransforms(simulation::Profile{}, q), vp, eye);
        std::vector<unsigned char> pixels{};
        pixels.resize(256 * 256 * 4);
        glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return pixels;
    } };
    const auto initial{ capture({ 0, 0, 30 }) };
    std::size_t visible{};
    for (std::size_t i{}; i < initial.size(); i += 4)
        if (initial[i] || initial[i + 1] || initial[i + 2])
            ++visible;
    require(visible > 500, "imported indexed model must produce actual visible fragments");
    for (const auto q :
         std::array<std::array<double, 3>, 3>{ { { .4, 0, 30 }, { 0, .5, 30 }, { 0, 0, 100 } } }) {
        const auto moved{ capture(q) };
        std::size_t changed{};
        for (std::size_t i{}; i < moved.size(); i += 4)
            if (moved[i] != initial[i] || moved[i + 1] != initial[i + 1] || moved[i + 2] != initial[i + 2])
                ++changed;
        require(changed > 50, "each of the three joint states must change rendered model pixels");
    }
    require(glGetError() == GL_NO_ERROR, "OpenGL errors");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::puts("PASS: OpenGL 4.5, shader diagnostics, RAII moves/deletion, transactional FBO resize and "
              "rendered pixels, Assimp indexed models and three-axis articulation");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
