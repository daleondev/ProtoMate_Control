#include "pch.h"

#include "Buffer.h"

VertexBuffer::VertexBuffer()
{
    glGenBuffers(1, &m_buffer);
}

VertexBuffer::~VertexBuffer()
{
    glDeleteBuffers(1, &m_buffer);
}

void VertexBuffer::allocate(const GLfloat* vertices, const size_t count)
{
    glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
    glBufferData(GL_ARRAY_BUFFER, count*sizeof(GLfloat), vertices, GL_STATIC_DRAW);
}

void VertexBuffer::bind() const
{
    glBindBuffer(GL_ARRAY_BUFFER, m_buffer);
}

void VertexBuffer::release() const
{
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ------------------------------------------------------

IndexBuffer::IndexBuffer() 
    : m_count(0)
{
    glGenBuffers(1, &m_buffer);
}

IndexBuffer::~IndexBuffer()
{
    glDeleteBuffers(1, &m_buffer);
}

void IndexBuffer::allocate(const GLuint* indices, const size_t count)
{
    m_count = count;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, count*sizeof(GLuint), indices, GL_STATIC_DRAW);
}

void IndexBuffer::bind() const
{
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_buffer);
}

void IndexBuffer::release() const
{
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}