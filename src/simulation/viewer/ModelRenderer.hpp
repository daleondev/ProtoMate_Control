#pragma once
#include "Gl.hpp"
#include "ModelAsset.hpp"

namespace simulation::viewer
{
    // Context-bound GPU ownership. Instances share uploaded geometry, retaining
    // all Assimp node transforms; only the three rigid part frames move.
    class ModelRenderer
    {
      public:
        ModelRenderer(const Context& context, const RobotModels& models);
        void draw(const std::array<glm::mat4, 3>& frames, const glm::mat4& view_projection, glm::vec3 eye);

      private:
        struct Geometry
        {
            Mesh mesh;
            glm::vec4 color;
        };
        struct Instance
        {
            std::size_t part{}, geometry{};
            glm::mat4 local{ 1 };
            glm::vec3 center{};
        };
        struct Draw
        {
            std::size_t geometry{};
            glm::mat4 transform{ 1 };
            float distance{};
        };
        Program m_program;
        GLint m_model{}, m_view{}, m_color{};
        std::vector<Geometry> m_geometry{};
        std::vector<Instance> m_instances{};
        std::vector<Draw> m_transparent{};
    };
}
