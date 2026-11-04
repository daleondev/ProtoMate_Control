#include "Viewer.hpp"
#include "../Session.hpp"
#include "../assets/EmbeddedAssets.hpp"
#include "Gl.hpp"
#include "ModelRenderer.hpp"
#include "Text.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <numbers>
#include <vector>

namespace simulation::viewer
{
    namespace
    {
        std::vector<Vertex> cube()
        {
            std::vector<Vertex> result{};
            for (unsigned axis{}; axis < 3; ++axis)
                for (int sign : { -1, 1 }) {
                    glm::vec3 n{};
                    n[axis] = static_cast<float>(sign);
                    std::array<glm::vec3, 4> p{};
                    for (unsigned i{}; i < 4; ++i) {
                        p[i][axis] = static_cast<float>(sign) * 0.5F;
                        p[i][(axis + 1) % 3] = (i == 1 || i == 2) ? 0.5F : -0.5F;
                        p[i][(axis + 2) % 3] = i >= 2 ? 0.5F : -0.5F;
                    }
                    for (unsigned i : { 0U, 1U, 2U, 0U, 2U, 3U })
                        result.push_back({ p[i], n });
                }
            return result;
        }
        std::vector<Vertex> cylinder()
        {
            std::vector<Vertex> result{};
            for (unsigned i{}; i < 48; ++i) {
                const float a{ float(i) * 2.0F * std::numbers::pi_v<float> / 48.0F },
                  b{ float(i + 1) * 2.0F * std::numbers::pi_v<float> / 48.0F };
                const glm::vec3 na{ std::cos(a), std::sin(a), 0 }, nb{ std::cos(b), std::sin(b), 0 };
                const glm::vec3 p0{ na.x * .5F, na.y * .5F, -.5F }, p1{ nb.x * .5F, nb.y * .5F, -.5F },
                  p2{ nb.x * .5F, nb.y * .5F, .5F }, p3{ na.x * .5F, na.y * .5F, .5F };
                result.insert(result.end(),
                              { { p0, na },
                                { p1, nb },
                                { p2, nb },
                                { p0, na },
                                { p2, nb },
                                { p3, na },
                                { { 0, 0, .5F }, { 0, 0, 1 } },
                                { p3, { 0, 0, 1 } },
                                { p2, { 0, 0, 1 } },
                                { { 0, 0, -.5F }, { 0, 0, -1 } },
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
    }
    void run(Session& session, const ViewerOptions& options)
    {
        const auto& profile{ session.profile() };
        const auto models{ RobotModels::load(options.model_directory, options.coordinates) };
        for (const auto& part : models.parts)
            for (const auto& warning : part.warnings)
                std::fprintf(stderr, "simulation model: %s\n", warning.c_str());
        std::fprintf(stderr,
                     "simulation models: %s (Assimp, three articulated parts)\n",
                     models.placeholders ? "embedded DUMMY geometry" : "external export");
        SdlSession sdl{};
        Window window{};
        Context context{ window };
        ModelRenderer robot{ context, models };
        const auto shader{ assets::sceneShader() };
        Program program{ context, shader.vertex, shader.fragment };
        const auto model_location{ program.location("model") },
          view_location{ program.location("viewProjection") }, color_location{ program.location("color") },
          unlit_location{ program.location("unlit") };
        const auto cube_vertices{ cube() }, cylinder_vertices{ cylinder() };
        Mesh box{ context, cube_vertices }, joint{ context, cylinder_vertices };
        std::vector<Vertex> lines{};
        for (int p{ -700 }; p <= 700; p += 50) {
            lines.push_back({ { float(p), -700, 0 }, { 0, 0, 1 } });
            lines.push_back({ { float(p), 700, 0 }, { 0, 0, 1 } });
            lines.push_back({ { -700, float(p), 0 }, { 0, 0, 1 } });
            lines.push_back({ { 700, float(p), 0 }, { 0, 0, 1 } });
        }
        Mesh grid{ context, lines, GL_LINES };
        Mesh overlay{ context, std::span<const Vertex>{} };
        auto wall_origin{ hostNow() };
        float yaw{ -.8F }, pitch{ .65F }, distance{ 1100 };
        glm::vec3 focus{ 150, 0, 100 };
        bool visible{ true };
        glEnable(GL_DEPTH_TEST);
        glClearColor(.045F, .06F, .085F, 1);
        while (visible && session.running()) {
            const auto frame_started{ hostNow() };
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT)
                    visible = false;
                if (event.type == SDL_MOUSEWHEEL)
                    distance = std::clamp(distance * std::pow(.9F, float(event.wheel.y)), 100.0F, 5000.0F);
                if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_LMASK)) {
                    yaw -= event.motion.xrel * .006F;
                    pitch = std::clamp(pitch + event.motion.yrel * .006F, .05F, 1.5F);
                }
                if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_RMASK)) {
                    focus.x -= event.motion.xrel;
                    focus.y += event.motion.yrel;
                }
                if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_r) {
                    yaw = -.8F;
                    pitch = .65F;
                    distance = 1100;
                    focus = { 150, 0, 100 };
                }
            }
            int width{}, height{};
            SDL_GL_GetDrawableSize(window.get(), &width, &height);
            if (width <= 0 || height <= 0) {
                SDL_Delay(20);
                continue;
            }
            const auto state{ session.snapshot() };
            std::array<char, 512> title{};
            std::snprintf(title.data(),
                          title.size(),
                          "ProtoMate | %s | %s (PROVISIONAL) | J %.2f %.2f deg Z %.2f mm | TCP %.1f %.1f "
                          "%.1f mm | %s | REF "
                          "%x FAULT %x | t %.3f s | drag orbit / right-drag pan / wheel zoom / R reset",
                          models.placeholders ? "DUMMY MODELS" : "EXTERNAL MODELS",
                          Profile::NAME,
                          state.joints[0] * 180 / std::numbers::pi,
                          state.joints[1] * 180 / std::numbers::pi,
                          state.joints[2],
                          state.tcp_mm[0],
                          state.tcp_mm[1],
                          state.tcp_mm[2],
                          state.enabled ? "ENABLED" : "DISABLED",
                          state.references,
                          state.faults,
                          state.time_ns / 1e9);
            SDL_SetWindowTitle(window.get(), title.data());
            const glm::vec3 eye{ focus + distance * glm::vec3{ std::cos(yaw) * std::cos(pitch),
                                                               std::sin(yaw) * std::cos(pitch),
                                                               std::sin(pitch) } };
            const auto vp{ glm::perspective(glm::radians(45.0F), float(width) / height, 1.0F, 10000.0F) *
                           glm::lookAt(eye, focus, glm::vec3{ 0, 0, 1 }) };
            glViewport(0, 0, width, height);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glUseProgram(program.get());
            glProgramUniformMatrix4fv(program.get(), view_location, 1, GL_FALSE, glm::value_ptr(vp));
            const auto draw{
                [&](const Mesh& mesh, const glm::mat4& matrix, glm::vec3 color, bool unlit = false) {
                glProgramUniformMatrix4fv(program.get(), model_location, 1, GL_FALSE, glm::value_ptr(matrix));
                glProgramUniform3fv(program.get(), color_location, 1, glm::value_ptr(color));
                glProgramUniform1i(program.get(), unlit_location, unlit);
                mesh.draw();
            }
            };
            draw(grid, glm::mat4{ 1 }, { .17F, .22F, .28F }, true);
            draw(box, transform({ 0, 0, 10 }, { 160, 160, 20 }), { .22F, .25F, .30F });
            draw(joint, transform({ 0, 0, 70 }, { 100, 100, 100 }), { .48F, .50F, .54F });
            robot.draw(robotModelTransforms(profile, state.joints, options.shoulder_height_mm), vp, eye);
            glUseProgram(program.get());
            const glm::vec3 tcp{ float(state.tcp_mm[0]), float(state.tcp_mm[1]), float(state.tcp_mm[2]) };
            draw(joint,
                 transform(tcp, { 10, 10, 20 }),
                 state.faults ? glm::vec3{ 1, .2F, .1F } : glm::vec3{ .2F, 1, .45F });
            for (unsigned i{}; i < 3; ++i) {
                glm::vec3 dimension{ 2, 2, 2 }, position{ 0, 0, 2 }, color{};
                dimension[i] = 120;
                position[i] = 60;
                color[i] = 1;
                draw(box, transform(position, dimension), color, true);
            }
            std::array<char, 1024> label{};
            const auto ratio{ double(state.time_ns) /
                              double(std::max<std::uint64_t>(1, hostNow() - wall_origin)) };
            std::snprintf(
              label.data(),
              label.size(),
              "PROTOMATE SIMULATED HARDWARE - %s\nGEOMETRY AND GEARING NOT CALIBRATED\nPOWER "
              "%s  DRIVERS %s  REF %X  FAULT %X  %s\nSHAFT DEG  %.3F  %.3F  %.3F\nJOINT DEG  %.3F  %.3F  Z "
              "%.3F MM\nTCP MM  X %.3F  Y %.3F  Z %.3F\nSIM TIME %.3F S   AVERAGE SPEED %.2FX\nNO DYNAMICS - "
              "NO CONTACT - NO LOAD MODEL\nDRAG ORBIT - RIGHT DRAG PAN - WHEEL ZOOM - R RESET",
              models.placeholders ? "DUMMY MODELS" : "EXTERNAL MODELS",
              state.powered ? "ON" : "OFF",
              state.enabled ? "ENABLED" : "DISABLED",
              state.references,
              state.faults,
              (hostNow() - state.published_host_ns > 500000000) ? "STALE" : "LIVE",
              state.shaft_degrees[0],
              state.shaft_degrees[1],
              state.shaft_degrees[2],
              state.joints[0] * 180 / std::numbers::pi,
              state.joints[1] * 180 / std::numbers::pi,
              state.joints[2],
              state.tcp_mm[0],
              state.tcp_mm[1],
              state.tcp_mm[2],
              state.time_ns / 1e9,
              ratio);
            const auto text{ textVertices(label.data(), 18, 18, 2) };
            overlay.update(text);
            glDisable(GL_DEPTH_TEST);
            const auto ortho{ glm::ortho(0.0F, float(width), float(height), 0.0F, -1.0F, 1.0F) };
            glProgramUniformMatrix4fv(program.get(), view_location, 1, GL_FALSE, glm::value_ptr(ortho));
            draw(overlay,
                 glm::mat4{ 1 },
                 (state.faults || hostNow() - state.published_host_ns > 500000000)
                   ? glm::vec3{ 1, .45F, .3F }
                   : glm::vec3{ .85F, .9F, .95F },
                 true);
            glEnable(GL_DEPTH_TEST);
            SDL_GL_SwapWindow(window.get());
            const auto elapsed{ hostNow() - frame_started };
            if (elapsed < 16'000'000)
                SDL_Delay(static_cast<Uint32>((16'000'000 - elapsed) / 1'000'000));
        }
    }
}
