#include "pch.h"

#include "Renderer.h"

#include "Scene/Camera.h"

RenderData Renderer::s_planeData;
RenderData Renderer::s_frameData;
RenderData Renderer::s_sphereData;
std::unordered_map<UUID, RenderData> Renderer::s_meshData;
std::unordered_map<UUID, RenderData> Renderer::s_boxData;

void Renderer::init()
{
    glEnable(GL_DEPTH_TEST);
    glClearDepthf(1.0f);
    glDepthFunc(GL_LESS);

    //------------------------------------------------------
    //                      Plane
    //------------------------------------------------------

    s_planeData.shader = ShaderLibrary::exists("Plane") ?   ShaderLibrary::get("Plane") :
                                                            ShaderLibrary::load("/home/david/Schreibtisch/RoboVis/src/Shaders/Plane", "Plane");

    s_planeData.vertexArray = std::make_shared<VertexArray>();
    s_planeData.vertexBuffer = std::make_shared<VertexBuffer>();
    s_planeData.vertexBuffer->allocate(reinterpret_cast<const float*>(PlaneData::vertices.data()), PlaneData::vertices.size() * sizeof(PlaneData::Vertex)/sizeof(float));
    s_planeData.vertexBuffer->setLayout(PlaneData::layout);
    s_planeData.vertexArray->addVertexBuffer(s_planeData.vertexBuffer);

    s_planeData.indexBuffer = std::make_shared<IndexBuffer>();
    s_planeData.indexBuffer->allocate(reinterpret_cast<const uint32_t*>(PlaneData::indices.data()), PlaneData::indices.size() * 3);
    s_planeData.vertexArray->setIndexBuffer(s_planeData.indexBuffer);

    //------------------------------------------------------
    //                      Frame
    //------------------------------------------------------

    s_frameData.shader = ShaderLibrary::exists("Frame") ?   ShaderLibrary::get("Frame") :
                                                            ShaderLibrary::load("/home/david/Schreibtisch/RoboVis/src/Shaders/Frame", "Frame");

    s_frameData.vertexArray = std::make_shared<VertexArray>();
    s_frameData.vertexBuffer = std::make_shared<VertexBuffer>();
    s_frameData.vertexBuffer->allocate(reinterpret_cast<const float*>(FrameData::vertices.data()), FrameData::vertices.size() * sizeof(FrameData::Vertex)/sizeof(float));
    s_frameData.vertexBuffer->setLayout(FrameData::layout);
    s_frameData.vertexArray->addVertexBuffer(s_frameData.vertexBuffer);

    s_frameData.indexBuffer = std::make_shared<IndexBuffer>();
    s_frameData.indexBuffer->allocate(reinterpret_cast<const uint32_t*>(FrameData::indices.data()), FrameData::indices.size() * 3);
    s_frameData.vertexArray->setIndexBuffer(s_frameData.indexBuffer);

    //------------------------------------------------------
    //                      Sphere
    //------------------------------------------------------

    s_sphereData.shader = ShaderLibrary::exists("Sphere") ?  ShaderLibrary::get("Sphere") :
                                                            ShaderLibrary::load("/home/david/Schreibtisch/RoboVis/src/Shaders/Sphere", "Sphere");

    const auto [vertices, indices] = SphereData::generateSphere();
    s_sphereData.vertexArray = std::make_shared<VertexArray>();
    s_sphereData.vertexBuffer = std::make_shared<VertexBuffer>();
    s_sphereData.vertexBuffer->allocate(reinterpret_cast<const float*>(vertices.data()), vertices.size() * sizeof(SphereData::Vertex)/sizeof(float));
    s_sphereData.vertexBuffer->setLayout(SphereData::layout);
    s_sphereData.vertexArray->addVertexBuffer(s_sphereData.vertexBuffer);

    s_sphereData.indexBuffer = std::make_shared<IndexBuffer>();
    s_sphereData.indexBuffer->allocate(reinterpret_cast<const uint32_t*>(indices.data()), indices.size() * 3);
    s_sphereData.vertexArray->setIndexBuffer(s_sphereData.indexBuffer);
}

void Renderer::shutdown()
{

}

void Renderer::clear(const glm::vec4& clearColor)
{
    glClearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::draw(const std::shared_ptr<Shader>& shader, const std::shared_ptr<VertexArray>& vertexArray)
{
    shader->bind();
    vertexArray->bind();
    glDrawElements(GL_TRIANGLES, vertexArray->getIndexBuffer()->getCount(), GL_UNSIGNED_INT, 0);
}

void Renderer::drawPlane(const glm::mat4& transform, const glm::vec4& color)
{
    const auto& camera = CameraController::getCamera();
    glm::mat4 mvp = camera.getProjection() * camera.getView() * glm::transpose(transform);

    s_planeData.shader->bind();
    s_planeData.shader->uploadMat4("u_mvp", mvp);
    s_planeData.shader->uploadVec4("u_color", color);
    s_planeData.shader->uploadInt("u_useTex", 0);
    draw(s_planeData.shader, s_planeData.vertexArray);
}

void Renderer::drawPlane(const glm::mat4& transform, const std::shared_ptr<Texture2D>& texture)
{
    const auto& camera = CameraController::getCamera();
    glm::mat4 mvp = camera.getProjection() * camera.getView() * glm::transpose(transform);

    s_planeData.shader->bind();
    s_planeData.shader->uploadMat4("u_mvp", mvp);
    s_planeData.shader->uploadInt("u_texture", texture->getSlot());
    s_planeData.shader->uploadInt("u_useTex", 1);
    texture->bind();
    draw(s_planeData.shader, s_planeData.vertexArray);
}

void Renderer::drawFrame(const glm::mat4& transform)
{
    const auto& camera = CameraController::getCamera();
    glm::mat4 mvp = camera.getProjection() * camera.getView() * glm::transpose(transform);

    s_frameData.shader->bind();
    s_frameData.shader->uploadMat4("u_mvp", mvp);
    draw(s_frameData.shader, s_frameData.vertexArray);
}

void Renderer::drawSphere(const glm::mat4& transform, const glm::vec4& color)
{
    const auto& camera = CameraController::getCamera();
    glm::mat4 mvp = camera.getProjection() * camera.getView() * glm::transpose(transform);

    s_sphereData.shader->bind();
    s_sphereData.shader->uploadMat4("u_mvp", mvp);
    s_sphereData.shader->uploadVec4("u_color", color);
    draw(s_sphereData.shader, s_sphereData.vertexArray);
}

void Renderer::drawMesh(const glm::mat4& transform, const UUID meshId)
{
    const auto& camera = CameraController::getCamera();
    glm::mat4 mvp = camera.getProjection() * camera.getView() * glm::transpose(transform);

    auto& meshData = s_meshData[meshId];
    meshData.shader->bind();
    meshData.shader->uploadMat4("u_mvp", mvp);
    draw(meshData.shader, meshData.vertexArray);
}

void Renderer::drawBox(const glm::mat4& transform, const UUID boxId, const glm::vec4& color)
{
    const auto& camera = CameraController::getCamera();
    glm::mat4 mvp = camera.getProjection() * camera.getView() * glm::transpose(transform);

    auto& boxData = s_boxData[boxId];
    boxData.shader->bind();
    boxData.shader->uploadMat4("u_mvp", mvp);
    s_sphereData.shader->uploadVec4("u_color", color);
    draw(boxData.shader, boxData.vertexArray);
}

void Renderer::addMeshData(const UUID meshId, const MeshData& data)
{
    RenderData meshData;
    meshData.shader = ShaderLibrary::exists("Mesh") ?   ShaderLibrary::get("Mesh") :
                                                        ShaderLibrary::load("/home/david/Schreibtisch/RoboVis/src/Shaders/Mesh", "Mesh");

    meshData.vertexArray = std::make_shared<VertexArray>();
    meshData.vertexBuffer = std::make_shared<VertexBuffer>();
    meshData.vertexBuffer->allocate(reinterpret_cast<const float*>(data.vertices.data()), data.vertices.size() * sizeof(MeshData::Vertex)/sizeof(float));
    meshData.vertexBuffer->setLayout(MeshData::layout);
    meshData.vertexArray->addVertexBuffer(meshData.vertexBuffer);

    meshData.indexBuffer = std::make_shared<IndexBuffer>();
    meshData.indexBuffer->allocate(reinterpret_cast<const uint32_t*>(data.indices.data()), data.indices.size() * 3);
    meshData.vertexArray->setIndexBuffer(meshData.indexBuffer);

    s_meshData.emplace(meshId, meshData);   
}

void Renderer::addBoxData(const UUID boxId, const BoxData& data)
{
    RenderData boxData;
    boxData.shader = ShaderLibrary::exists("Box") ? ShaderLibrary::get("Box") :
                                                    ShaderLibrary::load("/home/david/Schreibtisch/RoboVis/src/Shaders/Box", "Box");

    boxData.vertexArray = std::make_shared<VertexArray>();
    boxData.vertexBuffer = std::make_shared<VertexBuffer>();
    boxData.vertexBuffer->allocate(reinterpret_cast<const float*>(data.vertices.data()), data.vertices.size() * sizeof(BoxData::Vertex)/sizeof(float));
    boxData.vertexBuffer->setLayout(BoxData::layout);
    boxData.vertexArray->addVertexBuffer(boxData.vertexBuffer);

    boxData.indexBuffer = std::make_shared<IndexBuffer>();
    boxData.indexBuffer->allocate(reinterpret_cast<const uint32_t*>(BoxData::indices.data()), BoxData::indices.size() * 3);
    boxData.vertexArray->setIndexBuffer(boxData.indexBuffer);

    s_boxData.emplace(boxId, boxData);
}