#pragma once

enum class ShaderDataType {
    None = 0,
    Bool,
    Float,
    Float2,
    Float3,
    Float4,
    Float2x2,
    Float3x3,
    Float4x4,
    Int,
    Int2,
    Int3,
    Int4,
    Int2x2,
    Int3x3,
    Int4x4
};

static size_t shaderDataTypeSize(const ShaderDataType type)
{
    switch(type) {
        case ShaderDataType::Bool:       return sizeof(GLfloat);
        case ShaderDataType::Float:      return sizeof(GLfloat);
        case ShaderDataType::Float2:     return sizeof(GLuint)*2;
        case ShaderDataType::Float3:     return sizeof(GLfloat)*3;
        case ShaderDataType::Float4:     return sizeof(GLfloat)*4;
        case ShaderDataType::Float2x2:   return sizeof(GLfloat)*2*2;
        case ShaderDataType::Float3x3:   return sizeof(GLfloat)*3*3;
        case ShaderDataType::Float4x4:   return sizeof(GLfloat)*4*4;
        case ShaderDataType::Int:        return sizeof(GLint);
        case ShaderDataType::Int2:       return sizeof(GLint)*2;
        case ShaderDataType::Int3:       return sizeof(GLint)*3;
        case ShaderDataType::Int4:       return sizeof(GLint)*4;
        case ShaderDataType::Int2x2:     return sizeof(GLint)*2*2;
        case ShaderDataType::Int3x3:     return sizeof(GLint)*3*3;
        case ShaderDataType::Int4x4:     return sizeof(GLint)*4*4;
        case ShaderDataType::None:       return 0;
    }

    assert(false && "Unknown shader data type");
    return 0;
}

struct BufferElement {
    ShaderDataType type;
    std::string_view name;     
    size_t size;
    size_t offset;  
    bool normalized;

    BufferElement() = default;
    BufferElement(ShaderDataType type, const std::string_view& name, bool normalized = false)
        : type(type), name(name), size(shaderDataTypeSize(type)), offset(0), normalized(normalized)
    {       
    } 

    size_t getComponentCount() const
    {
        switch(type) {
            case ShaderDataType::Bool:       return 1;
            case ShaderDataType::Float:      return 1;
            case ShaderDataType::Float2:     return 2;
            case ShaderDataType::Float3:     return 3;
            case ShaderDataType::Float4:     return 4;
            case ShaderDataType::Float2x2:   return 2*2;
            case ShaderDataType::Float3x3:   return 3*3;
            case ShaderDataType::Float4x4:   return 4*4;
            case ShaderDataType::Int:        return 1;
            case ShaderDataType::Int2:       return 2;
            case ShaderDataType::Int3:       return 3;
            case ShaderDataType::Int4:       return 4;
            case ShaderDataType::Int2x2:     return 2*2;
            case ShaderDataType::Int3x3:     return 3*3;
            case ShaderDataType::Int4x4:     return 4*4;
            case ShaderDataType::None:       return 0;
        }

        assert(false && "Unknown shader data type");
        return 0;
    }
};

class BufferLayout 
{
public:
    typedef std::vector<BufferElement>::iterator BufferLayoutIterator;
    typedef std::vector<BufferElement>::const_iterator BufferLayoutConstIterator;

    BufferLayout() = default;
    BufferLayout(const std::initializer_list<BufferElement>& elements)
        : m_elements(elements), m_stride(0)
    {     
        calculateOffsetsAndStride();
    }

    inline const std::vector<BufferElement>& getElements() const { return m_elements; }
    inline size_t getStride() const { return m_stride; }

    inline BufferLayoutIterator begin() { return m_elements.begin(); }
    inline BufferLayoutIterator end() { return m_elements.end(); }
    inline BufferLayoutConstIterator begin() const { return m_elements.cbegin(); }
    inline BufferLayoutConstIterator end() const { return m_elements.cend(); }

private:
    void calculateOffsetsAndStride()
    {
        size_t offset = 0;
        m_stride = 0;
        for (auto& element : m_elements) {
            element.offset = offset;
            offset += element.size;
            m_stride += element.size;
        }
    }

    std::vector<BufferElement> m_elements;
    size_t m_stride;
};

class VertexBuffer 
{
public:
    VertexBuffer();
    ~VertexBuffer();

    void allocate(const GLfloat* vertices, const size_t count);

    void bind() const;
    void release() const;

    inline void setLayout(const BufferLayout& layout) { m_layout = layout; }
    inline const BufferLayout& getLayout() const { return m_layout; }

private: 
    GLuint m_buffer;
    BufferLayout m_layout;
};

class IndexBuffer  
{
public:
    IndexBuffer();
    ~IndexBuffer();

    void allocate(const GLuint* indices, const size_t count);

    void bind() const;
    void release() const;

    inline size_t getCount() const { return m_count; }

private: 
    GLuint m_buffer;
    size_t m_count;
};