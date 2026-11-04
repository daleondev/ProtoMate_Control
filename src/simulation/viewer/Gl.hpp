#pragma once
#include <SDL.h>
#include <array>
#include <cstddef>
#include <cstdio>
#include <epoxy/gl.h>
#include <glm/glm.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace simulation::viewer
{
    class SdlSession
    {
      public:
        SdlSession()
        {
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
                throw std::runtime_error{ SDL_GetError() };
        }
        ~SdlSession() { SDL_Quit(); }
        SdlSession(const SdlSession&) = delete;
        SdlSession& operator=(const SdlSession&) = delete;
    };
    class Window
    {
      public:
        Window()
        {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
            m_window = SDL_CreateWindow("ProtoMate simulated machine",
                                        SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED,
                                        1280,
                                        800,
                                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
            if (!m_window)
                throw std::runtime_error{ SDL_GetError() };
        }
        ~Window() { SDL_DestroyWindow(m_window); }
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        SDL_Window* get() const noexcept { return m_window; }

      private:
        SDL_Window* m_window{};
    };
    class Context
    {
      public:
        explicit Context(Window& window)
          : m_owner{ std::this_thread::get_id() }
        {
            m_context = SDL_GL_CreateContext(window.get());
            if (!m_context)
                throw std::runtime_error{ SDL_GetError() };
            if (epoxy_gl_version() < 45) {
                SDL_GL_DeleteContext(m_context);
                m_context = nullptr;
                throw std::runtime_error{ "OpenGL 4.5 core is required" };
            }
            glEnable(GL_DEBUG_OUTPUT);
            glDebugMessageCallback(
              [](GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*) {
                if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH)
                    std::fprintf(stderr, "OpenGL: %s\n", message);
            }, nullptr);
            SDL_GL_SetSwapInterval(1);
        }
        ~Context()
        {
            requireOwner();
            SDL_GL_DeleteContext(m_context);
        }
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        void requireOwner() const noexcept
        {
            if (m_owner != std::this_thread::get_id() || SDL_GL_GetCurrentContext() != m_context)
                std::terminate();
        }
        void check() const
        {
            if (m_owner != std::this_thread::get_id() || SDL_GL_GetCurrentContext() != m_context)
                throw std::logic_error{ "OpenGL context/thread ownership" };
        }

      private:
        SDL_GLContext m_context{};
        std::thread::id m_owner;
    };
    enum class Kind
    {
        Buffer,
        VertexArray,
        Texture,
        Renderbuffer,
        Framebuffer
    };
    template<Kind K>
    class Object
    {
      public:
        explicit Object(const Context& context)
          : m_context{ &context }
        {
            context.check();
            if constexpr (K == Kind::Buffer)
                glCreateBuffers(1, &m_id);
            if constexpr (K == Kind::VertexArray)
                glCreateVertexArrays(1, &m_id);
            if constexpr (K == Kind::Texture)
                glCreateTextures(GL_TEXTURE_2D, 1, &m_id);
            if constexpr (K == Kind::Renderbuffer)
                glCreateRenderbuffers(1, &m_id);
            if constexpr (K == Kind::Framebuffer)
                glCreateFramebuffers(1, &m_id);
            if (!m_id)
                throw std::runtime_error{ "OpenGL object allocation failed" };
        }
        ~Object() noexcept { reset(); }
        Object(const Object&) = delete;
        Object& operator=(const Object&) = delete;
        Object(Object&& other) noexcept
          : m_context{ other.m_context }
          , m_id{ std::exchange(other.m_id, 0) }
        {
        }
        Object& operator=(Object&& other) noexcept
        {
            if (this != &other) {
                reset();
                m_context = other.m_context;
                m_id = std::exchange(other.m_id, 0);
            }
            return *this;
        }
        GLuint get() const
        {
            m_context->check();
            return m_id;
        }

      private:
        void reset() noexcept
        {
            if (!m_id)
                return;
            m_context->requireOwner();
            if constexpr (K == Kind::Buffer)
                glDeleteBuffers(1, &m_id);
            if constexpr (K == Kind::VertexArray)
                glDeleteVertexArrays(1, &m_id);
            if constexpr (K == Kind::Texture)
                glDeleteTextures(1, &m_id);
            if constexpr (K == Kind::Renderbuffer)
                glDeleteRenderbuffers(1, &m_id);
            if constexpr (K == Kind::Framebuffer)
                glDeleteFramebuffers(1, &m_id);
            m_id = 0;
        }
        const Context* m_context;
        GLuint m_id{};
    };
    using Buffer = Object<Kind::Buffer>;
    using VertexArray = Object<Kind::VertexArray>;
    using Texture = Object<Kind::Texture>;
    using Renderbuffer = Object<Kind::Renderbuffer>;
    using Framebuffer = Object<Kind::Framebuffer>;

    class Shader
    {
      public:
        Shader(const Context& context, GLenum stage, const char* text)
          : m_context{ &context }
        {
            context.check();
            m_id = glCreateShader(stage);
            if (!m_id)
                throw std::runtime_error{ "shader allocation" };
            try {
                glShaderSource(m_id, 1, &text, nullptr);
                glCompileShader(m_id);
                GLint valid{};
                glGetShaderiv(m_id, GL_COMPILE_STATUS, &valid);
                if (!valid) {
                    GLint count{};
                    glGetShaderiv(m_id, GL_INFO_LOG_LENGTH, &count);
                    auto log{ std::string(static_cast<std::size_t>(std::max(count, 1)), '\0') };
                    glGetShaderInfoLog(m_id, count, nullptr, log.data());
                    glDeleteShader(m_id);
                    m_id = 0;
                    throw std::runtime_error{ log };
                }
            } catch (...) {
                if (m_id)
                    glDeleteShader(m_id);
                m_id = 0;
                throw;
            }
        }
        ~Shader() noexcept
        {
            if (m_id) {
                m_context->requireOwner();
                glDeleteShader(m_id);
            }
        }
        Shader(Shader&& other) noexcept
          : m_context{ other.m_context }
          , m_id{ std::exchange(other.m_id, 0) }
        {
        }
        Shader(const Shader&) = delete;
        Shader& operator=(const Shader&) = delete;
        GLuint get() const noexcept { return m_id; }

      private:
        const Context* m_context;
        GLuint m_id{};
    };
    class Program
    {
      public:
        Program(const Context& context, const char* vertex, const char* fragment)
          : m_context{ &context }
        {
            context.check();
            const Shader vs{ context, GL_VERTEX_SHADER, vertex }, fs{ context, GL_FRAGMENT_SHADER, fragment };
            m_id = glCreateProgram();
            if (!m_id)
                throw std::runtime_error{ "program allocation" };
            try {
                glAttachShader(m_id, vs.get());
                glAttachShader(m_id, fs.get());
                glLinkProgram(m_id);
                GLint valid{};
                glGetProgramiv(m_id, GL_LINK_STATUS, &valid);
                if (!valid) {
                    GLint count{};
                    glGetProgramiv(m_id, GL_INFO_LOG_LENGTH, &count);
                    auto log{ std::string(static_cast<std::size_t>(std::max(count, 1)), '\0') };
                    glGetProgramInfoLog(m_id, count, nullptr, log.data());
                    glDeleteProgram(m_id);
                    m_id = 0;
                    throw std::runtime_error{ log };
                }
                glDetachShader(m_id, vs.get());
                glDetachShader(m_id, fs.get());
            } catch (...) {
                if (m_id)
                    glDeleteProgram(m_id);
                m_id = 0;
                throw;
            }
        }
        ~Program() noexcept
        {
            if (m_id) {
                m_context->requireOwner();
                glDeleteProgram(m_id);
            }
        }
        Program(const Program&) = delete;
        Program& operator=(const Program&) = delete;
        Program(Program&& other) noexcept
          : m_context{ other.m_context }
          , m_id{ std::exchange(other.m_id, 0) }
        {
        }
        GLuint get() const noexcept { return m_id; }
        GLint location(const char* name) const
        {
            m_context->check();
            const auto result{ glGetUniformLocation(m_id, name) };
            if (result < 0)
                throw std::runtime_error{ std::string{ "missing shader uniform: " } + name };
            return result;
        }

      private:
        const Context* m_context;
        GLuint m_id{};
    };
    class RenderTarget
    {
      public:
        RenderTarget(const Context& context, int width, int height)
          : m_frame{ context }
          , m_color{ context }
          , m_depth{ context }
        {
            if (width <= 0 || height <= 0)
                throw std::invalid_argument{ "invalid render target dimensions" };
            glTextureStorage2D(m_color.get(), 1, GL_RGBA8, width, height);
            glNamedRenderbufferStorage(m_depth.get(), GL_DEPTH_COMPONENT24, width, height);
            glNamedFramebufferTexture(m_frame.get(), GL_COLOR_ATTACHMENT0, m_color.get(), 0);
            glNamedFramebufferRenderbuffer(
              m_frame.get(), GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth.get());
            if (glCheckNamedFramebufferStatus(m_frame.get(), GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error{ "incomplete render target" };
        }
        void resize(const Context& context, int width, int height)
        {
            RenderTarget replacement{ context, width, height };
            std::swap(m_frame, replacement.m_frame);
            std::swap(m_color, replacement.m_color);
            std::swap(m_depth, replacement.m_depth);
        }
        GLuint get() const noexcept { return m_frame.get(); }

      private:
        Framebuffer m_frame;
        Texture m_color;
        Renderbuffer m_depth;
    };
    struct Vertex
    {
        glm::vec3 position{}, normal{};
    };
    class Mesh
    {
      public:
        Mesh(const Context& context, std::span<const Vertex> vertices, GLenum mode = GL_TRIANGLES)
          : m_vertices{ context }
          , m_array{ context }
          , m_count{ static_cast<GLsizei>(vertices.size()) }
          , m_mode{ mode }
        {
            if (vertices.size() > 1'000'000)
                throw std::length_error{ "mesh vertex count" };
            static_assert(std::is_standard_layout_v<Vertex>);
            glNamedBufferData(m_vertices.get(),
                              static_cast<GLsizeiptr>(vertices.size_bytes()),
                              vertices.data(),
                              GL_STATIC_DRAW);
            glVertexArrayVertexBuffer(m_array.get(), 0, m_vertices.get(), 0, sizeof(Vertex));
            for (GLuint i{}; i < 2; ++i) {
                glEnableVertexArrayAttrib(m_array.get(), i);
                glVertexArrayAttribFormat(m_array.get(),
                                          i,
                                          3,
                                          GL_FLOAT,
                                          GL_FALSE,
                                          i ? offsetof(Vertex, normal) : offsetof(Vertex, position));
                glVertexArrayAttribBinding(m_array.get(), i, 0);
            }
        }
        void update(std::span<const Vertex> vertices)
        {
            if (vertices.size() > 1'000'000)
                throw std::length_error{ "mesh vertex count" };
            m_count = static_cast<GLsizei>(vertices.size());
            glNamedBufferData(m_vertices.get(),
                              static_cast<GLsizeiptr>(vertices.size_bytes()),
                              vertices.data(),
                              GL_DYNAMIC_DRAW);
        }
        void draw() const
        {
            glBindVertexArray(m_array.get());
            glDrawArrays(m_mode, 0, m_count);
        }

      private:
        Buffer m_vertices;
        VertexArray m_array;
        GLsizei m_count{};
        GLenum m_mode{};
    };
}
