#pragma once

#include <GLES3/gl3.h>

#include <glm/glm.hpp>

#include <string>
#include <memory>
#include <unordered_map>
#include <filesystem>

class Shader {

public:
    Shader(const std::string& name = "");
    ~Shader();

    bool add(const GLenum type, const GLchar* source);
    bool addFromFile(const GLenum type, const std::filesystem::path& filePath);
    bool link();

    void bind() const;
    void release() const;

    [[nodiscard]] GLint attributeLocation(const std::string& name) const;
    void enableAttributeArray(const GLint location);
    void setAttributeBuffer(const GLint location, const GLint size, const GLenum type, const GLboolean normalized, const GLsizei stride, const GLint offset);

    void uploadInt(const std::string& name, const int i) const;
    void uploadFloat(const std::string& name, const float f) const;

    void uploadVec2(const std::string& name, const glm::vec2& vec) const;
    void uploadVec3(const std::string& name, const glm::vec3& vec) const;
    void uploadVec4(const std::string& name, const glm::vec4& vec) const;

    void uploadMat3(const std::string& name, const glm::mat3& mat) const;
    void uploadMat4(const std::string& name, const glm::mat4& mat) const;

    inline void setName(const std::string& name) { m_name = name; };
    inline const std::string& getName() const { return m_name; };

private:
    bool compile(const GLenum type, const GLchar* source);
    GLint getUniformLocation(const std::string& name) const;

    std::string m_name;
    GLuint m_fragmentShader;
    GLuint m_vertexShader;
    GLuint m_program;
};

class ShaderLibrary
{
public:
    static void add(const std::shared_ptr<Shader>& shader);
    static std::shared_ptr<Shader> load(const std::string& filepath, const std::string& name = "");
    static std::shared_ptr<Shader> load(const std::string& name, const std::string& vertSource, const std::string& fragSource);

    static std::shared_ptr<Shader> get(const std::string& name);
    inline static bool exists(const std::string& name) { return s_shaders.find(name) != s_shaders.end(); }

private:
    static std::unordered_map<std::string, std::shared_ptr<Shader>> s_shaders;
};