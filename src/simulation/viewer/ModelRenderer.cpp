#include "ModelRenderer.hpp"
#include "../assets/EmbeddedAssets.hpp"
#include <algorithm>
#include <glm/gtc/type_ptr.hpp>

namespace simulation::viewer
{
    namespace
    {
        constexpr float OPAQUE_ALPHA{ .999F };
    }
    ModelRenderer::ModelRenderer(const Context& context, const RobotModels& models)
      : m_program{ context, assets::model_shader() }
      , m_model{ m_program.location("model") }
      , m_view{ m_program.location("viewProjection") }
      , m_color{ m_program.location("color") }
    {
        for (std::size_t part{}; part < models.parts.size(); ++part) {
            const auto& asset{ models.parts[part] };
            const auto offset{ m_geometry.size() };
            for (const auto& geometry : asset.geometries) {
                m_geometry.push_back(
                  { Mesh{ context, geometry.vertices, geometry.indices }, geometry.color });
            }
            for (const auto& instance : asset.instances) {
                m_instances.push_back(
                  { part, offset + instance.geometry, instance.transform, instance.center });
            }
        }
        m_transparent.reserve(m_instances.size());
    }
    void ModelRenderer::draw(const std::array<glm::mat4, 4>& frames,
                             const glm::mat4& view_projection,
                             glm::vec3 eye)
    {
        glUseProgram(m_program.get());
        glProgramUniformMatrix4fv(m_program.get(), m_view, 1, GL_FALSE, glm::value_ptr(view_projection));
        const auto render{ [&](const Draw& item) {
            const auto& geometry{ m_geometry[item.geometry] };
            glProgramUniformMatrix4fv(m_program.get(), m_model, 1, GL_FALSE, glm::value_ptr(item.transform));
            glProgramUniform4fv(m_program.get(), m_color, 1, glm::value_ptr(geometry.color));
            geometry.mesh.draw();
        } };
        // Opaque first; sort translucent instances back-to-front across all parts.
        // Intersecting transparent CAD solids are not order-independent transparency.
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        m_transparent.clear();
        for (const auto& instance : m_instances) {
            const auto& geometry{ m_geometry[instance.geometry] };
            const auto frame{ frames[instance.part] };
            const auto delta{ glm::vec3{ frame * glm::vec4{ instance.center, 1 } } - eye };
            const Draw item{ .geometry = instance.geometry,
                             .transform = frame * instance.local,
                             .distance = glm::dot(delta, delta) };
            if (geometry.color[3] >= OPAQUE_ALPHA) {
                render(item);
            }
            else if (geometry.color[3] > 0) {
                m_transparent.push_back(item);
            }
        }
        std::ranges::sort(m_transparent,
                          [](const Draw& a, const Draw& b) { return a.distance > b.distance; });
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (const auto& item : m_transparent) {
            render(item);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
}
