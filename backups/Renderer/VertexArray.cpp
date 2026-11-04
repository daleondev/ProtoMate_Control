#include "pch.h"

#include "VertexArray.h"

static GLenum ShaderDataTypeToOpenGLBaseType(const ShaderDataType type)
{
    switch (type)
    {
        case ShaderDataType::Bool:       return GL_BOOL;
        case ShaderDataType::Float:      return GL_FLOAT;
        case ShaderDataType::Float2:     return GL_FLOAT;
        case ShaderDataType::Float3:     return GL_FLOAT;
        case ShaderDataType::Float4:     return GL_FLOAT;
        case ShaderDataType::Float2x2:   return GL_FLOAT;
        case ShaderDataType::Float3x3:   return GL_FLOAT;
        case ShaderDataType::Float4x4:   return GL_FLOAT;
        case ShaderDataType::Int:        return GL_INT;
        case ShaderDataType::Int2:       return GL_INT;
        case ShaderDataType::Int3:       return GL_INT;
        case ShaderDataType::Int4:       return GL_INT;
        case ShaderDataType::Int2x2:     return GL_INT;
        case ShaderDataType::Int3x3:     return GL_INT;
        case ShaderDataType::Int4x4:     return GL_INT;
        case ShaderDataType::None:       return GL_NONE;
    }

    assert(false && "Unknown shader data type");
    return 0;
}

VertexArray::VertexArray()
{
    glGenVertexArrays(1, &m_array);
}

VertexArray::~VertexArray()
{
    glDeleteVertexArrays(1, &m_array);
}

void VertexArray::bind() const
{
    glBindVertexArray(m_array);
}

void VertexArray::release() const
{
    glBindVertexArray(0);
}

void VertexArray::addVertexBuffer(const std::shared_ptr<VertexBuffer>& vertexBuffer)
{
    const auto& layout = vertexBuffer->getLayout();
    assert(!layout.getElements().empty() && "Vertex buffer has no layout!");

    glBindVertexArray(m_array);
    vertexBuffer->bind();     

    size_t index = 0;
    for (const auto& element : layout) {
        glEnableVertexAttribArray(index);
        glVertexAttribPointer(  index++, element.getComponentCount(), ShaderDataTypeToOpenGLBaseType(element.type), 
                                (GLboolean)element.normalized, layout.getStride(), (const void*)element.offset);
    }

    m_vertexBuffers.push_back(vertexBuffer);
}

void VertexArray::setIndexBuffer(const std::shared_ptr<IndexBuffer>& indexBuffer)
{
    glBindVertexArray(m_array);
    indexBuffer->bind();

    m_indexBuffer = indexBuffer;
}