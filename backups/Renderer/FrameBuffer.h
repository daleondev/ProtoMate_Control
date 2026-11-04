#pragma once

class FrameBuffer 
{
public:
    FrameBuffer(const uint16_t width, const uint16_t height);
    ~FrameBuffer();

    void resize(const uint16_t width, const uint16_t height);

    void bind() const;
    void release() const;

    inline GLuint getColorAttachment() const { return m_colorAttachment; }

private: 
    uint16_t m_width;
    uint16_t m_height;

    GLuint m_colorAttachment;
    GLuint m_depthAttachment;
    GLuint m_buffer;

};