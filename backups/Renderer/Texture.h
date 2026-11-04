#pragma once

class Texture2D {

public:
    Texture2D(const std::string& path);
    Texture2D(const GLubyte* data, const GLuint width, const GLuint height);
    ~Texture2D();

    void bind(const GLuint slot = 0);
    void release() const;

    void setMinificationFilter(const GLuint type);
    void setMagnificationFilter(const GLuint type);
    void setWrapMode(const GLuint mode);

    virtual uint32_t getWidth() const { return m_width; }
    virtual uint32_t getHeight() const { return m_height; }

    inline GLuint getId() const { return m_texture; }
    inline GLuint getSlot() const { return m_slot; }

private:
    uint32_t m_width;
    uint32_t m_height;
    
    GLuint m_texture;
    GLuint m_slot = -1;
    
};

class TextureLibrary
{
public:
    static void add(const std::string& name, const std::shared_ptr<Texture2D>& texture);
    static std::shared_ptr<Texture2D> load(const std::string& filepath, const std::string& name = "");
    static std::shared_ptr<Texture2D> create(const std::string& name, const std::vector<uint32_t> data, const uint32_t width, const uint32_t height);

    static std::shared_ptr<Texture2D> get(const std::string& name);
    inline static bool exists(const std::string& name) { return s_textures.find(name) != s_textures.end(); }

private:
    static std::unordered_map<std::string, std::shared_ptr<Texture2D>> s_textures;
};