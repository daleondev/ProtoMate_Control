#include "ModelRenderer.hpp"
#include "../assets/EmbeddedAssets.hpp"
#include <algorithm>
#include <glm/gtc/type_ptr.hpp>

namespace simulation::viewer
{
    ModelRenderer::ModelRenderer(const Context& context, const RobotModels& models)
      : m_program{ context, assets::modelShader().vertex, assets::modelShader().fragment }
      , m_model{ m_program.location("model") }
      , m_view{ m_program.location("viewProjection") }
      , m_color{ m_program.location("color") }
    {
        for (std::size_t part{}; part < models.parts.size(); ++part) {
            const auto& asset{ models.parts[part] };
            const auto offset{ m_geometry.size() };
            for (const auto& geometry : asset.geometries)
                m_geometry.push_back(
                  { Mesh{ context, geometry.vertices, geometry.indices }, geometry.color });
            for (const auto& instance : asset.instances)
                m_instances.push_back(
                  { part, offset + instance.geometry, instance.transform, instance.center });
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
            const Draw item{ instance.geometry, frame * instance.local, glm::dot(delta, delta) };
            if (geometry.color.a >= .999F)
                render(item);
            else if (geometry.color.a > 0)
                m_transparent.push_back(item);
        }
        std::sort(m_transparent.begin(), m_transparent.end(), [](const Draw& a, const Draw& b) {
            return a.distance > b.distance;
        });
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (const auto& item : m_transparent)
            render(item);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
}
