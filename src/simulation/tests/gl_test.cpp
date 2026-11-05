#include "../viewer/Gl.hpp"
#include "../viewer/ModelRenderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <utility>
using namespace simulation::viewer;
class SimulationOpenGL : public testing::Test
{
  protected:
    SdlSession sdl;
    Window window;
    Context context{ window };
    void TearDown() override
    {
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
};

namespace
{
    static constexpr char vertex[]{
#embed "shaders/test.vert"
    };
    static constexpr char fragment[]{
#embed "shaders/test.frag"
    };
    static constexpr char invalid_source[]{
#embed "shaders/invalid.vert"
    };
}

TEST_F(SimulationOpenGL, ShaderCompilerDiagnostics)
{
    bool failed{};
    try {
        Shader invalid{ context, GL_VERTEX_SHADER, { invalid_source, sizeof(invalid_source) } };
    } catch (const std::runtime_error& e) {
        failed = std::string_view{ e.what() }.size() > 10;
    }
    ASSERT_TRUE((failed)) << "shader errors must include compiler log";
}

TEST_F(SimulationOpenGL, ProgramMoveOwnership)
{
    Program original{ context, { vertex, sizeof(vertex) }, { fragment, sizeof(fragment) } };
    const auto id{ original.get() };
    Program program{ std::move(original) };
    ASSERT_TRUE((program.get() == id)) << "program move ownership";
}

TEST_F(SimulationOpenGL, BufferMoveAndDestruction)
{
    Buffer buffer{ context };
    const auto buffer_id{ buffer.get() };
    {
        Buffer moved{ std::move(buffer) };
        ASSERT_TRUE((glIsBuffer(moved.get()))) << "buffer moved alive";
    }
    ASSERT_TRUE((!glIsBuffer(buffer_id))) << "buffer destruction releases object";
}

TEST_F(SimulationOpenGL, InvalidResizePreservesFramebuffer)
{
    RenderTarget target{ context, 32, 32 };
    target.resize(context, 64, 64);
    bool dimensions{};
    try {
        target.resize(context, 0, 64);
    } catch (const std::invalid_argument&) {
        dimensions = true;
    }
    ASSERT_TRUE((dimensions)) << "invalid resize rejected";
    ASSERT_TRUE(glIsFramebuffer(target.get()));
}

TEST_F(SimulationOpenGL, RejectsCrossThreadAccess)
{
    Program program{ context, { vertex, sizeof(vertex) }, { fragment, sizeof(fragment) } };
    RenderTarget target{ context, 64, 64 };
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
    ASSERT_TRUE((rejected_accesses == 2))
      << "cross-thread GL access must throw, not terminate or return a handle";
}

TEST_F(SimulationOpenGL, ShaderDrawReadback)
{
    Program program{ context, { vertex, sizeof(vertex) }, { fragment, sizeof(fragment) } };
    RenderTarget target{ context, 64, 64 };
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
    ASSERT_TRUE((pixel[0] >= 50 && pixel[0] <= 52 && pixel[1] >= 203 && pixel[1] <= 205 && pixel[2] >= 101 &&
                 pixel[2] <= 103))
      << "actual GLSL draw/readback";
}

TEST_F(SimulationOpenGL, IndexedRobotAndIndependentJointRendering)
{
    RenderTarget target{ context, 256, 256 };
    glClearColor(0, 0, 0, 1);
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
    ASSERT_TRUE((visible > 500)) << "imported indexed model must produce actual visible fragments";
    for (const auto q :
         std::array<std::array<double, 3>, 3>{ { { .4, 0, 30 }, { 0, .5, 30 }, { 0, 0, 100 } } }) {
        const auto moved{ capture(q) };
        std::size_t changed{};
        for (std::size_t i{}; i < moved.size(); i += 4)
            if (moved[i] != initial[i] || moved[i + 1] != initial[i + 1] || moved[i + 2] != initial[i + 2])
                ++changed;
        ASSERT_TRUE((changed > 50)) << "each of the three joint states must change rendered model pixels";
    }
    ASSERT_TRUE((glGetError() == GL_NO_ERROR)) << "OpenGL errors";
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
