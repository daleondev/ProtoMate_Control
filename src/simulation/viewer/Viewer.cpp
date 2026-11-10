#include "Viewer.hpp"
#include "../Session.hpp"
#include "../assets/EmbeddedAssets.hpp"
#include "Gl.hpp"
#include "ModelRenderer.hpp"
#include "Text.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <numbers>
#include <vector>

namespace simulation::viewer
{
    namespace
    {
        constexpr float HALF{ .5F }, DEFAULT_YAW{ -.8F }, DEFAULT_PITCH{ .65F }, DEFAULT_DISTANCE{ 1400 },
          WHEEL_SCALE{ .9F }, MIN_DISTANCE{ 100 }, MAX_DISTANCE{ 5000 }, ORBIT_SCALE{ .006F },
          MIN_PITCH{ .05F }, MAX_PITCH{ 1.5F }, FOV_DEGREES{ 45 }, FAR_CLIP{ 10000 };
        constexpr glm::vec3 DEFAULT_FOCUS{ 180, 0, 200 }, BACKGROUND{ .045F, .06F, .085F },
          GRID_COLOR{ .17F, .22F, .28F }, FAULT_COLOR{ 1, .2F, .1F }, READY_COLOR{ .2F, 1, .45F },
          WARNING_COLOR{ 1, .45F, .3F }, TEXT_COLOR{ .85F, .9F, .95F }, TCP_SIZE{ 10, 10, 20 };
        constexpr unsigned CYLINDER_SIDES{ 48 }, TITLE_CAPACITY{ 512 }, LABEL_CAPACITY{ 1024 },
          MINIMIZED_DELAY_MS{ 20 }, LABEL_PADDING{ 18 };
        constexpr int GRID_EXTENT{ 700 }, GRID_SPACING{ 50 }, AXIS_LENGTH{ 120 }, AXIS_CENTRE{ 60 };
        constexpr double DEGREES_PER_RADIAN{ 180 / std::numbers::pi }, NS_PER_SECOND{ 1e9 };
        constexpr std::uint64_t STALE_NS{ 500'000'000 }, FRAME_NS{ 16'000'000 }, NS_PER_MS{ 1'000'000 };
        struct Camera
        {
            float yaw{ DEFAULT_YAW }, pitch{ DEFAULT_PITCH }, distance{ DEFAULT_DISTANCE };
            glm::vec3 focus{ DEFAULT_FOCUS };
        };
        // SDL's event tag determines the active member of its C union.
        // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access)
        bool input(const SDL_Event& event, Camera& camera)
        {
            switch (event.type) {
                case SDL_QUIT:
                    return false;
                case SDL_MOUSEWHEEL:
                    camera.distance =
                      std::clamp(camera.distance * std::pow(WHEEL_SCALE, float(event.wheel.y)),
                                 MIN_DISTANCE,
                                 MAX_DISTANCE);
                    break;
                case SDL_MOUSEMOTION:
                    if (event.motion.state & SDL_BUTTON_LMASK) {
                        camera.yaw -= static_cast<float>(event.motion.xrel) * ORBIT_SCALE;
                        camera.pitch =
                          std::clamp(camera.pitch + (static_cast<float>(event.motion.yrel) * ORBIT_SCALE),
                                     MIN_PITCH,
                                     MAX_PITCH);
                    }
                    if (event.motion.state & SDL_BUTTON_RMASK) {
                        camera.focus[0] -= static_cast<float>(event.motion.xrel);
                        camera.focus[1] += static_cast<float>(event.motion.yrel);
                    }
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_r) {
                        camera = {};
                    }
                    break;
                default:
                    break;
            }
            return true;
        }
        // NOLINTEND(cppcoreguidelines-pro-type-union-access)
        template<class... Args>
        void format(std::span<char> output, std::format_string<Args...> pattern, Args&&... args)
        {
            const auto result{ std::format_to_n(
              output.data(), output.size() - 1, pattern, std::forward<Args>(args)...) };
            *result.out = '\0';
        }
        std::vector<Vertex> cube()
        {
            std::vector<Vertex> result{};
            for (glm::length_t axis{}; axis < 3; ++axis) {
                for (int sign : { -1, 1 }) {
                    glm::vec3 n{};
                    n[axis] = static_cast<float>(sign);
                    std::array<glm::vec3, 4> p{};
                    for (unsigned i{}; i < 4; ++i) {
                        p[i][axis] = static_cast<float>(sign) * HALF;
                        p[i][(axis + 1) % 3] = (i == 1 || i == 2) ? HALF : -HALF;
                        p[i][(axis + 2) % 3] = i >= 2 ? HALF : -HALF;
                    }
                    for (unsigned i : { 0U, 1U, 2U, 0U, 2U, 3U }) {
                        result.push_back({ p[i], n });
                    }
                }
            }
            return result;
        }
        std::vector<Vertex> cylinder()
        {
            std::vector<Vertex> result{};
            for (unsigned i{}; i < CYLINDER_SIDES; ++i) {
                const float a{ float(i) * 2.0F * std::numbers::pi_v<float> / float(CYLINDER_SIDES) };
                const float b{ float(i + 1) * 2.0F * std::numbers::pi_v<float> / float(CYLINDER_SIDES) };
                const glm::vec3 na{ std::cos(a), std::sin(a), 0 };
                const glm::vec3 nb{ std::cos(b), std::sin(b), 0 };
                const glm::vec3 p0{ na[0] * HALF, na[1] * HALF, -HALF };
                const glm::vec3 p1{ nb[0] * HALF, nb[1] * HALF, -HALF };
                const glm::vec3 p2{ nb[0] * HALF, nb[1] * HALF, HALF };
                const glm::vec3 p3{ na[0] * HALF, na[1] * HALF, HALF };
                result.insert(result.end(),
                              { { p0, na },
                                { p1, nb },
                                { p2, nb },
                                { p0, na },
                                { p2, nb },
                                { p3, na },
                                { { 0, 0, HALF }, { 0, 0, 1 } },
                                { p3, { 0, 0, 1 } },
                                { p2, { 0, 0, 1 } },
                                { { 0, 0, -HALF }, { 0, 0, -1 } },
                                { p0, { 0, 0, -1 } },
                                { p1, { 0, 0, -1 } } });
            }
            return result;
        }
        glm::mat4 transform(glm::vec3 position, glm::vec3 dimensions, float yaw = 0)
        {
            return glm::scale(
              glm::rotate(glm::translate(glm::mat4{ 1 }, position), yaw, glm::vec3{ 0, 0, 1 }), dimensions);
        }

        void report_models(const RobotModels& models)
        {
            for (const auto& part : models.parts) {
                for (const auto& warning : part.warnings) {
                    // Keep failure diagnostics nonthrowing, including allocation-failure paths.
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
                    std::fprintf(stderr, "simulation model: %s\n", warning.c_str());
                }
            }
            // Keep failure diagnostics nonthrowing, including allocation-failure paths.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,modernize-use-std-print)
            std::fprintf(stderr,
                         "simulation models: %s (Assimp, fixed base + three articulated links)\n",
                         models.external ? "external Fusion CAD" : "embedded Fusion CAD");
        }
        std::vector<Vertex> grid_vertices()
        {
            std::vector<Vertex> lines{};
            for (int p{ -GRID_EXTENT }; p <= GRID_EXTENT; p += GRID_SPACING) {
                lines.push_back({ { float(p), -GRID_EXTENT, 0 }, { 0, 0, 1 } });
                lines.push_back({ { float(p), GRID_EXTENT, 0 }, { 0, 0, 1 } });
                lines.push_back({ { -GRID_EXTENT, float(p), 0 }, { 0, 0, 1 } });
                lines.push_back({ { GRID_EXTENT, float(p), 0 }, { 0, 0, 1 } });
            }
            return lines;
        }
        template<class Snapshot>
        void update_title(const Window& window, bool external, const Snapshot& state)
        {
            std::array<char, TITLE_CAPACITY> title{};
            format(title,
                   "ProtoMate | {} | {} (PROVISIONAL) | J {:.2f} {:.2f} deg Z {:.2f} mm | TCP {:.1f} {:.1f} "
                   "{:.1f} mm | {} | REF "
                   "{:x} FAULT {:x} | t {:.3f} s | drag orbit / right-drag pan / wheel zoom / R reset",
                   external ? "EXTERNAL CAD" : "FUSION CAD",
                   Profile::NAME,
                   state.joints[0] * DEGREES_PER_RADIAN,
                   state.joints[1] * DEGREES_PER_RADIAN,
                   state.joints[2],
                   state.tcp_mm[0],
                   state.tcp_mm[1],
                   state.tcp_mm[2],
                   state.enabled ? "ENABLED" : "DISABLED",
                   state.references,
                   state.faults,
                   static_cast<double>(state.time_ns) / NS_PER_SECOND);
            SDL_SetWindowTitle(window.get(), title.data());
        }
    }
    void run(Session& session, const ViewerOptions& options)
    {
        const auto& profile{ session.profile() };
        const auto models{ RobotModels::load(options.model_directory, options.coordinates) };
        report_models(models);
        SdlSession sdl{};
        Window window{};
        Context context{ window };
        ModelRenderer robot{ context, models };
        const auto shader{ assets::scene_shader() };
        Program program{ context, shader };
        const auto model_location{ program.location("model") };
        const auto view_location{ program.location("viewProjection") };
        const auto color_location{ program.location("color") };
        const auto unlit_location{ program.location("unlit") };
        const auto cube_vertices{ cube() };
        const auto cylinder_vertices{ cylinder() };
        Mesh box{ context, cube_vertices };
        Mesh joint{ context, cylinder_vertices };
        const auto lines{ grid_vertices() };
        Mesh grid{ context, lines, GL_LINES };
        Mesh overlay{ context, std::span<const Vertex>{} };
        auto wall_origin{ host_now() };
        Camera camera{};
        bool visible{ true };
        glEnable(GL_DEPTH_TEST);
        glClearColor(BACKGROUND[0], BACKGROUND[1], BACKGROUND[2], 1);
        while (visible && session.running()) {
            const auto frame_started{ host_now() };
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                visible = input(event, camera) && visible;
            }
            int width{};
            int height{};
            SDL_GL_GetDrawableSize(window.get(), &width, &height);
            if (width <= 0 || height <= 0) {
                SDL_Delay(MINIMIZED_DELAY_MS);
                continue;
            }
            const auto state{ session.snapshot() };
            update_title(window, models.external, state);
            const auto& [yaw, pitch, distance, focus]{ camera };
            const glm::vec3 eye{ focus + distance * glm::vec3{ std::cos(yaw) * std::cos(pitch),
                                                               std::sin(yaw) * std::cos(pitch),
                                                               std::sin(pitch) } };
            const auto vp{ glm::perspective(glm::radians(FOV_DEGREES),
                                            float(width) / static_cast<float>(height),
                                            1.0F,
                                            FAR_CLIP) *
                           glm::lookAt(eye, focus, glm::vec3{ 0, 0, 1 }) };
            glViewport(0, 0, width, height);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glUseProgram(program.get());
            glProgramUniformMatrix4fv(program.get(), view_location, 1, GL_FALSE, glm::value_ptr(vp));
            const auto draw{
                [&program, model_location, color_location, unlit_location](
                  const Mesh& mesh, const glm::mat4& matrix, glm::vec3 color, bool unlit = false) {
                glProgramUniformMatrix4fv(program.get(), model_location, 1, GL_FALSE, glm::value_ptr(matrix));
                glProgramUniform3fv(program.get(), color_location, 1, glm::value_ptr(color));
                glProgramUniform1i(program.get(), unlit_location, unlit);
                mesh.draw();
            }
            };
            draw(grid, glm::mat4{ 1 }, GRID_COLOR, true);
            robot.draw(robot_model_transforms(profile, state.joints), vp, eye);
            glUseProgram(program.get());
            const glm::vec3 tcp{ float(state.tcp_mm[0]), float(state.tcp_mm[1]), float(state.tcp_mm[2]) };
            draw(joint, transform(tcp, TCP_SIZE), state.faults ? FAULT_COLOR : READY_COLOR);
            for (glm::length_t i{}; i < 3; ++i) {
                glm::vec3 dimension{ 2, 2, 2 };
                glm::vec3 position{ 0, 0, 2 };
                glm::vec3 color{};
                dimension[i] = AXIS_LENGTH;
                position[i] = AXIS_CENTRE;
                color[i] = 1;
                draw(box, transform(position, dimension), color, true);
            }
            std::array<char, LABEL_CAPACITY> label{};
            const auto ratio{ double(state.time_ns) /
                              double(std::max<std::uint64_t>(1, host_now() - wall_origin)) };
            format(label,
                   "PROTOMATE SIMULATED HARDWARE - {}\nNOMINAL CAD - HOMING NOT CALIBRATED\nPOWER "
                   "{}  DRIVERS {}  REF {:X}  FAULT {:X}  {}\nSHAFT DEG  {:.3f}  {:.3f}  {:.3f}\nJOINT DEG  "
                   "{:.3f}  {:.3f}  Z "
                   "{:.3f} MM\nTCP MM  X {:.3f}  Y {:.3f}  Z {:.3f}\nSIM TIME {:.3f} S   AVERAGE SPEED "
                   "{:.2f}X\nNO DYNAMICS - "
                   "SWITCH CONTACTS ONLY - NO LOAD MODEL\nDRAG ORBIT - RIGHT DRAG PAN - WHEEL ZOOM - R RESET",
                   models.external ? "EXTERNAL CAD" : "FUSION CAD",
                   state.powered ? "ON" : "OFF",
                   state.enabled ? "ENABLED" : "DISABLED",
                   state.references,
                   state.faults,
                   (host_now() - state.published_host_ns > STALE_NS) ? "STALE" : "LIVE",
                   state.shaft_degrees[0],
                   state.shaft_degrees[1],
                   state.shaft_degrees[2],
                   state.joints[0] * DEGREES_PER_RADIAN,
                   state.joints[1] * DEGREES_PER_RADIAN,
                   state.joints[2],
                   state.tcp_mm[0],
                   state.tcp_mm[1],
                   state.tcp_mm[2],
                   static_cast<double>(state.time_ns) / NS_PER_SECOND,
                   ratio);
            const auto text{ text_vertices(label.data(), LABEL_PADDING, LABEL_PADDING, 2) };
            overlay.update(text);
            glDisable(GL_DEPTH_TEST);
            const auto ortho{ glm::ortho(0.0F, float(width), float(height), 0.0F, -1.0F, 1.0F) };
            glProgramUniformMatrix4fv(program.get(), view_location, 1, GL_FALSE, glm::value_ptr(ortho));
            draw(overlay,
                 glm::mat4{ 1 },
                 (state.faults || host_now() - state.published_host_ns > STALE_NS) ? WARNING_COLOR
                                                                                   : TEXT_COLOR,
                 true);
            glEnable(GL_DEPTH_TEST);
            SDL_GL_SwapWindow(window.get());
            const auto elapsed{ host_now() - frame_started };
            if (elapsed < FRAME_NS) {
                SDL_Delay(static_cast<Uint32>((FRAME_NS - elapsed) / NS_PER_MS));
            }
        }
    }
}
