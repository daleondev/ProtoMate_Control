#pragma once

#include "RenderData.h"

#include "Util/util.h"

class Renderer {

public:
    static void init();
    static void shutdown();

    static void clear(const glm::vec4& clearColor);

    static void draw(const std::shared_ptr<Shader>& shader, const std::shared_ptr<VertexArray>& vertexArray);
    static void drawPlane(const glm::mat4& transform, const glm::vec4& color);
    static void drawPlane(const glm::mat4& transform, const std::shared_ptr<Texture2D>& texture);
    static void drawFrame(const glm::mat4& transform);
    static void drawSphere(const glm::mat4& transform, const glm::vec4& color);
    static void drawMesh(const glm::mat4& transform, const UUID meshId);
    static void drawBox(const glm::mat4& transform, const UUID boxId, const glm::vec4& color);

    static void addMeshData(const UUID meshId, const MeshData& data);
    static void addBoxData(const UUID boxId, const BoxData& data);

private:
    static RenderData s_planeData;
    static RenderData s_frameData;
    static RenderData s_sphereData;
    static std::unordered_map<UUID, RenderData> s_meshData;
    static std::unordered_map<UUID, RenderData> s_boxData;

};
