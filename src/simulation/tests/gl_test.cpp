#include "../viewer/Gl.hpp"
#include <cstdio>
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
    const char* vertex{ R"(#version 450 core
layout(location=0) in vec3 position;
void main(){gl_Position=vec4(position,1);})" };
    const char* fragment{ R"(#version 450 core
out vec4 color;
void main(){color=vec4(0.2,0.8,0.4,1);})" };
    bool failed{};
    try {
        Shader invalid{ context, GL_VERTEX_SHADER, "#version 450 core\ninvalid shader" };
    } catch (const std::runtime_error& e) {
        failed = std::string_view{ e.what() }.size() > 10;
    }
    require(failed, "shader errors must include compiler log");
    Program original{ context, vertex, fragment };
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
    require(glGetError() == GL_NO_ERROR, "OpenGL errors");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::puts("PASS: OpenGL 4.5, shader diagnostics, RAII moves/deletion, transactional FBO resize and "
              "rendered pixels");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
