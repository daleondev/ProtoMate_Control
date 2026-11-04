#include "pch.h"

#include "Shader.h"

Shader::Shader(const std::string& name)
    : m_name(name)
{
    m_program = glCreateProgram();
}

Shader::~Shader()
{
    glDeleteProgram(m_program);
}

bool Shader::add(const GLenum type, const GLchar* source)
{
    assert(!m_name.empty() && "No shader-name specified");
    return compile(type, source);
}

bool Shader::addFromFile(const GLenum type, const std::filesystem::path& filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open()) {
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();

    if (m_name.empty())
        m_name = filePath.filename().stem().string();

    return compile(type, ss.str().c_str());
}

void Shader::bind() const
{
    glUseProgram(m_program);
}

void Shader::release() const
{
    glUseProgram(0);
}

GLint Shader::attributeLocation(const std::string& name) const
{
    return glGetAttribLocation(m_program, name.c_str());
}

void Shader::enableAttributeArray(const GLint location)
{
    if (location != -1) {
        glEnableVertexAttribArray(location);
    }
}

void Shader::setAttributeBuffer(const GLint location, const GLint size, const GLenum type, const GLboolean normalized, const GLsizei stride, const GLint offset)
{
    if (location != -1) {
        glVertexAttribPointer(location, size, type, normalized, stride, reinterpret_cast<const void*>(offset));
    }
}

void Shader::uploadInt(const std::string& name, const int i) const
{
    GLint location = glGetUniformLocation(m_program, name.c_str());
    if (location == -1) {
        std::cerr << "Uniform " << name << " doesnt exist\n";
        return;
    }

    glUniform1i(location, i);
}

void Shader::uploadFloat(const std::string& name, const float f) const
{
    GLint location = glGetUniformLocation(m_program, name.c_str());
    if (location == -1) {
        std::cerr << "Uniform " << name << " doesnt exist\n";
        return;
    }

    glUniform1f(location, f);
}

void Shader::uploadVec2(const std::string& name, const glm::vec2& vec) const
{
    GLint location = glGetUniformLocation(m_program, name.c_str());
    if (location == -1) {
        std::cerr << "Uniform " << name << " doesnt exist\n";
        return;
    }

    glUniform2fv(location, 1, glm::value_ptr(vec));
}

void Shader::uploadVec3(const std::string& name, const glm::vec3& vec) const
{
    GLint location = glGetUniformLocation(m_program, name.c_str());
    if (location == -1) {
        std::cerr << "Uniform " << name << " doesnt exist\n";
        return;
    }

    glUniform3fv(location, 1, glm::value_ptr(vec));
}

void Shader::uploadVec4(const std::string& name, const glm::vec4& vec) const
{
    GLint location = glGetUniformLocation(m_program, name.c_str());
    if (location == -1) {
        std::cerr << "Uniform " << name << " doesnt exist\n";
        return;
    }

    glUniform4fv(location, 1, glm::value_ptr(vec));
}

void Shader::uploadMat3(const std::string& name, const glm::mat3& mat) const
{
    GLint location = glGetUniformLocation(m_program, name.c_str());
    if (location == -1) {
        std::cerr << "Uniform " << name << " doesnt exist\n";
        return;
    }

    glUniformMatrix3fv(location, 1, GL_FALSE, glm::value_ptr(mat));
}

void Shader::uploadMat4(const std::string& name, const glm::mat4& mat) const
{
    GLint location = glGetUniformLocation(m_program, name.c_str());
    if (location == -1) {
        std::cerr << "Uniform " << name << " doesnt exist\n";
        return;
    }

    glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(mat));
}

bool Shader::compile(const GLenum type, const GLchar* source)
{
    GLuint shader;

    shader = glCreateShader(type);

    glShaderSource(shader, 1, &source, 0);

    glCompileShader(shader);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);

        std::vector<GLchar> msg(length);
        glGetShaderInfoLog(shader, length, &length, msg.data());

        glDeleteShader(shader);

        std::cerr << "Compilation failed: " << msg.data() << '\n';
        return false;
    }

    glAttachShader(m_program, shader);

    switch (type) {
    case GL_FRAGMENT_SHADER:
    {
        m_fragmentShader = shader;
        break;
    }
    case GL_VERTEX_SHADER:
    {
        m_vertexShader = shader;
        break;
    }
    }

    return true;
}

bool Shader::link()
{
    glLinkProgram(m_program);

    GLint linked = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        GLint length = 0;
        glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &length);

        std::vector<GLchar> msg(length);
        glGetProgramInfoLog(m_program, length, &length, msg.data());

        glDeleteProgram(m_program);

        glDeleteShader(m_fragmentShader);
        glDeleteShader(m_vertexShader);

        std::cerr << "Compilation failed: " << msg.data() << '\n';

        return false;
    }

    glDetachShader(m_program, m_fragmentShader);
    glDetachShader(m_program, m_vertexShader);
    glDeleteShader(m_fragmentShader);
    glDeleteShader(m_vertexShader);

    return true;
}

GLint Shader::getUniformLocation(const std::string& name) const
{
    GLint location = glGetUniformLocation(m_program, name.c_str());
    if (location == -1) {
        std::cerr << "Uniform " << name << " doesnt exist" << '\n';
    }

    return location;
}

// --------------------------------------------------

std::unordered_map<std::string, std::shared_ptr<Shader>> ShaderLibrary::s_shaders;

void ShaderLibrary::add(const std::shared_ptr<Shader>& shader)
{
    const auto name = shader->getName();
    assert(s_shaders.find(name) == s_shaders.end() && "shader already exists");
    s_shaders[name] = shader;
}

std::shared_ptr<Shader> ShaderLibrary::load(const std::string& filepath, const std::string& name)
{
    std::shared_ptr<Shader> shader = std::make_shared<Shader>();
    shader->setName(name);

    shader->addFromFile(GL_VERTEX_SHADER, filepath + ".vert");
    shader->addFromFile(GL_FRAGMENT_SHADER, filepath + ".frag");
    shader->link();

    add(shader);
    return shader;
}

std::shared_ptr<Shader> ShaderLibrary::load(const std::string& name, const std::string& vertSource, const std::string& fragSource)
{
    std::shared_ptr<Shader> shader = std::make_shared<Shader>();
    shader->setName(name);

    shader->add(GL_VERTEX_SHADER, vertSource.c_str());
    shader->add(GL_FRAGMENT_SHADER, fragSource.c_str());
    shader->link();

    add(shader);
    return shader;
}

std::shared_ptr<Shader> ShaderLibrary::get(const std::string& name)
{
    assert(s_shaders.find(name) != s_shaders.end() && "shader not found");
    return s_shaders[name];
}