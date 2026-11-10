#pragma once
#include "../assets/EmbeddedAssets.hpp"
#include "Geometry.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <epoxy/gl.h>
#include <glm/glm.hpp>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace simulation::viewer
{
    inline constexpr int GL_MAJOR{ 4 }, GL_MINOR{ 5 }, REQUIRED_GL_VERSION{ (GL_MAJOR * 10) + GL_MINOR };
    inline constexpr std::size_t GPU_VERTEX_LIMIT{ 2'000'000 }, GPU_INDEX_LIMIT{ 6'000'000 };
    class SdlSession
    {
      public:
        SdlSession(SdlSession&&) = delete;
        auto operator=(SdlSession&&) -> SdlSession& = delete;

        SdlSession()
        {
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
                const std::string error{ SDL_GetError() };
                SDL_Quit();
                throw std::runtime_error{ error };
            }
        }
        ~SdlSession() { SDL_Quit(); }
        SdlSession(const SdlSession&) = delete;
        SdlSession& operator=(const SdlSession&) = delete;
    };
    class Window
    {
      public:
        Window(Window&&) = delete;
        auto operator=(Window&&) -> Window& = delete;

        Window()
          : m_window{ create() }
        {
        }
        ~Window() { SDL_DestroyWindow(m_window); }
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        SDL_Window* get() const noexcept { return m_window; }

      private:
        static SDL_Window* create()
        {
            constexpr int width{ 1280 };
            constexpr int height{ 800 };
            constexpr int depth_bits{ 24 };
            // Pixel format/context attributes must precede window creation.
            for (const auto [attribute, value] :
                 std::array{ std::pair{ SDL_GL_CONTEXT_MAJOR_VERSION, GL_MAJOR },
                             std::pair{ SDL_GL_CONTEXT_MINOR_VERSION, GL_MINOR },
                             std::pair{ SDL_GL_CONTEXT_PROFILE_MASK, int(SDL_GL_CONTEXT_PROFILE_CORE) },
                             std::pair{ SDL_GL_DOUBLEBUFFER, 1 },
                             std::pair{ SDL_GL_DEPTH_SIZE, depth_bits } }) {
                if (SDL_GL_SetAttribute(attribute, value) != 0) {
                    throw std::runtime_error{ SDL_GetError() };
                }
            }
            auto* window{ SDL_CreateWindow("ProtoMate simulated machine",
                                           SDL_WINDOWPOS_CENTERED,
                                           SDL_WINDOWPOS_CENTERED,
                                           width,
                                           height,
                                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                             SDL_WINDOW_ALLOW_HIGHDPI) };
            if (!window) {
                throw std::runtime_error{ SDL_GetError() };
            }
            return window;
        }
        SDL_Window* m_window{};
    };
    class Context
    {
      public:
        Context(Context&&) = delete;
        auto operator=(Context&&) -> Context& = delete;

        explicit Context(Window& window)
          : m_context{ SDL_GL_CreateContext(window.get()) }
          , m_owner{ std::this_thread::get_id() }
        {

            if (!m_context) {
                throw std::runtime_error{ SDL_GetError() };
            }
            if (epoxy_gl_version() < REQUIRED_GL_VERSION) {
                SDL_GL_DeleteContext(m_context);
                m_context = nullptr;
                throw std::runtime_error{ "OpenGL 4.5 core is required" };
            }
            glEnable(GL_DEBUG_OUTPUT);
            glDebugMessageCallback(
              [](GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*) {
                if (type == GL_DEBUG_TYPE_ERROR || severity == GL_DEBUG_SEVERITY_HIGH) {
                    // Keep failure diagnostics nonthrowing, including allocation-failure paths.
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
                    std::fprintf(stderr, "OpenGL: %s\n", message);
                }
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
            if (m_owner != std::this_thread::get_id() || SDL_GL_GetCurrentContext() != m_context) {
                std::terminate();
            }
        }
        void check() const
        {
            if (m_owner != std::this_thread::get_id() || SDL_GL_GetCurrentContext() != m_context) {
                throw std::logic_error{ "OpenGL context/thread ownership" };
            }
        }

      private:
        SDL_GLContext m_context{};
        std::thread::id m_owner;
    };
    enum class Kind : std::uint8_t
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
            if constexpr (K == Kind::Buffer) {
                glCreateBuffers(1, &m_id);
            }
            if constexpr (K == Kind::VertexArray) {
                glCreateVertexArrays(1, &m_id);
            }
            if constexpr (K == Kind::Texture) {
                glCreateTextures(GL_TEXTURE_2D, 1, &m_id);
            }
            if constexpr (K == Kind::Renderbuffer) {
                glCreateRenderbuffers(1, &m_id);
            }
            if constexpr (K == Kind::Framebuffer) {
                glCreateFramebuffers(1, &m_id);
            }
            if (!m_id) {
                throw std::runtime_error{ "OpenGL object allocation failed" };
            }
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
            if (!m_id) {
                return;
            }
            m_context->requireOwner();
            if constexpr (K == Kind::Buffer) {
                glDeleteBuffers(1, &m_id);
            }
            if constexpr (K == Kind::VertexArray) {
                glDeleteVertexArrays(1, &m_id);
            }
            if constexpr (K == Kind::Texture) {
                glDeleteTextures(1, &m_id);
            }
            if constexpr (K == Kind::Renderbuffer) {
                glDeleteRenderbuffers(1, &m_id);
            }
            if constexpr (K == Kind::Framebuffer) {
                glDeleteFramebuffers(1, &m_id);
            }
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
        auto operator=(Shader&&) -> Shader& = delete;

        Shader(const Context& context, GLenum stage, std::string_view text)
          : m_context{ &context }
        {
            context.check();
            if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<GLint>::max())) {
                throw std::invalid_argument{ "invalid shader source length" };
            }
            m_id = glCreateShader(stage);
            if (!m_id) {
                throw std::runtime_error{ "shader allocation" };
            }
            try {
                const auto* source{ text.data() };
                const auto length{ static_cast<GLint>(text.size()) };
                glShaderSource(m_id, 1, &source, &length);
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
                if (m_id) {
                    glDeleteShader(m_id);
                }
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
        GLuint get() const
        {
            m_context->check();
            return m_id;
        }

      private:
        const Context* m_context;
        GLuint m_id{};
    };
    class Program
    {
      public:
        auto operator=(Program&&) -> Program& = delete;

        Program(const Context& context, assets::ShaderSources sources)
          : m_context{ &context }
          , m_id{ create(context, sources) }
        {
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
        GLuint get() const
        {
            m_context->check();
            return m_id;
        }
        GLint location(const char* name) const
        {
            m_context->check();
            const auto result{ glGetUniformLocation(m_id, name) };
            if (result < 0) {
                throw std::runtime_error{ std::string{ "missing shader uniform: " } + name };
            }
            return result;
        }

      private:
        static GLuint create(const Context& context, assets::ShaderSources sources)
        {
            context.check();
            // Compile first: failed compilation must not leak an allocated program.
            const Shader vs{ context, GL_VERTEX_SHADER, sources.vertex };
            const Shader fs{ context, GL_FRAGMENT_SHADER, sources.fragment };
            GLuint id{ glCreateProgram() };
            if (!id) {
                throw std::runtime_error{ "program allocation" };
            }
            try {
                glAttachShader(id, vs.get());
                glAttachShader(id, fs.get());
                glLinkProgram(id);
                GLint valid{};
                glGetProgramiv(id, GL_LINK_STATUS, &valid);
                if (!valid) {
                    GLint count{};
                    glGetProgramiv(id, GL_INFO_LOG_LENGTH, &count);
                    auto log{ std::string(static_cast<std::size_t>(std::max(count, 1)), '\0') };
                    glGetProgramInfoLog(id, count, nullptr, log.data());
                    glDeleteProgram(id);
                    id = 0;
                    throw std::runtime_error{ log };
                }
                glDetachShader(id, vs.get());
                glDetachShader(id, fs.get());
            } catch (...) {
                if (id) {
                    glDeleteProgram(id);
                }
                id = 0;
                throw;
            }
            return id;
        }

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
            if (width <= 0 || height <= 0) {
                throw std::invalid_argument{ "invalid render target dimensions" };
            }
            glTextureStorage2D(m_color.get(), 1, GL_RGBA8, width, height);
            glNamedRenderbufferStorage(m_depth.get(), GL_DEPTH_COMPONENT24, width, height);
            glNamedFramebufferTexture(m_frame.get(), GL_COLOR_ATTACHMENT0, m_color.get(), 0);
            glNamedFramebufferRenderbuffer(
              m_frame.get(), GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth.get());
            if (glCheckNamedFramebufferStatus(m_frame.get(), GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                throw std::runtime_error{ "incomplete render target" };
            }
        }
        void resize(const Context& context, int width, int height)
        {
            RenderTarget replacement{ context, width, height };
            std::swap(m_frame, replacement.m_frame);
            std::swap(m_color, replacement.m_color);
            std::swap(m_depth, replacement.m_depth);
        }
        GLuint get() const { return m_frame.get(); }

      private:
        Framebuffer m_frame;
        Texture m_color;
        Renderbuffer m_depth;
    };
    class Mesh
    {
      public:
        Mesh(const Context& context, std::span<const Vertex> vertices, GLenum mode = GL_TRIANGLES)
          : m_vertices{ context }
          , m_indices{ context }
          , m_array{ context }
          , m_count{ static_cast<GLsizei>(vertices.size()) }
          , m_mode{ mode }
        {
            if (vertices.size() > GPU_VERTEX_LIMIT) {
                throw std::length_error{ "mesh vertex count" };
            }
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
        Mesh(const Context& context, std::span<const Vertex> vertices, std::span<const std::uint32_t> indices)
          : Mesh{ context, vertices }
        {
            if (indices.empty() || indices.size() > GPU_INDEX_LIMIT || indices.size() % 3) {
                throw std::length_error{ "invalid triangle index count" };
            }
            for (auto index : indices) {
                if (index >= vertices.size()) {
                    throw std::out_of_range{ "mesh vertex index" };
                }
            }
            glNamedBufferData(
              m_indices.get(), static_cast<GLsizeiptr>(indices.size_bytes()), indices.data(), GL_STATIC_DRAW);
            glVertexArrayElementBuffer(m_array.get(), m_indices.get());
            m_count = static_cast<GLsizei>(indices.size());
            m_indexed = true;
        }
        void update(std::span<const Vertex> vertices)
        {
            if (m_indexed) {
                throw std::logic_error{ "cannot stream-update an indexed mesh" };
            }
            if (vertices.size() > GPU_VERTEX_LIMIT) {
                throw std::length_error{ "mesh vertex count" };
            }
            m_count = static_cast<GLsizei>(vertices.size());
            glNamedBufferData(m_vertices.get(),
                              static_cast<GLsizeiptr>(vertices.size_bytes()),
                              vertices.data(),
                              GL_DYNAMIC_DRAW);
        }
        void draw() const
        {
            glBindVertexArray(m_array.get());
            if (m_indexed) {
                glDrawElements(m_mode, m_count, GL_UNSIGNED_INT, nullptr);
            }
            else {
                glDrawArrays(m_mode, 0, m_count);
            }
        }

      private:
        Buffer m_vertices, m_indices;
        VertexArray m_array;
        GLsizei m_count{};
        GLenum m_mode{};
        bool m_indexed{};
    };
}
