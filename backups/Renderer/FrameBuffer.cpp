#include "pch.h"

#include "FrameBuffer.h"

FrameBuffer::FrameBuffer(const uint16_t width, const uint16_t height)
    : m_width(width), m_height(height)
{
    resize(width, height);
}

FrameBuffer::~FrameBuffer()
{
    glDeleteFramebuffers(1, &m_buffer);
    glDeleteTextures(1, &m_colorAttachment);
}

void FrameBuffer::resize(const uint16_t width, const uint16_t height)
{
    if (m_buffer) {
        glDeleteFramebuffers(1, &m_buffer);
        glDeleteTextures(1, &m_colorAttachment);
        glDeleteRenderbuffers(1, &m_depthAttachment);
    }

    glGenFramebuffers(1, &m_buffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_buffer);

    glGenTextures(1, &m_colorAttachment);
    glBindTexture(GL_TEXTURE_2D, m_colorAttachment);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);	
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_colorAttachment, 0);

    glGenRenderbuffers(1, &m_depthAttachment);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depthAttachment);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthAttachment);

    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && "Framebuffer incomplete");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

    // glGenTextures(GL_TEXTURE_2D, &m_depthAttachment);
    // glBindTexture(GL_TEXTURE_2D, m_depthAttachment);
    // glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, width, height);
    // glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, m_depthAttachment, 0);

void FrameBuffer::bind() const
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_buffer);
}

void FrameBuffer::release() const
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}