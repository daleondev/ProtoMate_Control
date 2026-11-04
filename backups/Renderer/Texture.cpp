#include "pch.h"

#include "Texture.h"

Texture2D::Texture2D(const std::string& path)
    : m_texture(0)
{
    int width, height, channels;
    stbi_set_flip_vertically_on_load(1);
    stbi_uc* data = stbi_load(path.c_str(), &width, &height, &channels, 0);
    assert(data && "Failed to load image");

    m_width = width;
    m_height= height;

    GLenum internalFormat = 0;
    GLenum dataFormat = 0;
    switch(channels) {
        case 3:
            internalFormat = GL_RGB;
            dataFormat = GL_RGB;
            break;
        case 4:
            internalFormat = GL_RGBA;
            dataFormat = GL_RGBA;
            break;
        default:
            assert(false && "Invalid format");
    }

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);	
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, dataFormat, GL_UNSIGNED_BYTE, data);

    const auto isPowerOfTwo = [](const uint32_t x) -> bool { return (x & (x - 1)) == 0; };
    if (isPowerOfTwo(width) && isPowerOfTwo(height)) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(data);
}

Texture2D::Texture2D(const GLubyte* data, const GLuint width, const GLuint height)
    : m_texture(0)
{
    m_width = width;
    m_height= height;

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);	
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    const auto isPowerOfTwo = [](const uint32_t x) -> bool { return (x & (x - 1)) == 0; };
    if (isPowerOfTwo(width) && isPowerOfTwo(height)) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
}

Texture2D::~Texture2D()
{
    glDeleteTextures(1, &m_texture);
}

void Texture2D::bind(const GLuint slot)
{
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    m_slot = slot;
}

void Texture2D::release() const
{
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Texture2D::setMinificationFilter(const GLuint type)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, type);
}

void Texture2D::setMagnificationFilter(const GLuint type)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, type);
}

void Texture2D::setWrapMode(const GLuint mode)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mode);
}

// --------------------------------------------------

std::unordered_map<std::string, std::shared_ptr<Texture2D>> TextureLibrary::s_textures;

void TextureLibrary::add(const std::string& name, const std::shared_ptr<Texture2D>& texture)
{
    assert(s_textures.find(name) == s_textures.end() && "texture already exists");
    s_textures[name] = texture;
}

std::shared_ptr<Texture2D> TextureLibrary::load(const std::string& filepath, const std::string& name)
{
    std::shared_ptr<Texture2D> texture = std::make_shared<Texture2D>(filepath);
    add(name, texture);
    return texture;
}

std::shared_ptr<Texture2D> TextureLibrary::create(const std::string& name, const std::vector<uint32_t> data, const uint32_t width, const uint32_t height)
{
    std::shared_ptr<Texture2D> texture = std::make_shared<Texture2D>((GLubyte*)data.data(), width, height);
    add(name, texture);
    return texture;
}

std::shared_ptr<Texture2D> TextureLibrary::get(const std::string& name)
{
    assert(s_textures.find(name) != s_textures.end() && "texture not found");
    return s_textures[name];
}