#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <string>

#include <glad/glad.h>

namespace andromeda::app::rendering {

class Buffer {
public:
    Buffer() = default;
    explicit Buffer(GLenum target);
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    [[nodiscard]] GLuint id() const { return buffer_; }

    void bind() const;

    template <typename D>
    void set_data(std::span<const D> data, GLenum usage) const {
        bind();
        glBufferData(target_, static_cast<GLsizeiptr>(data.size_bytes()), data.data(), usage);
    }

    template <typename D>
    void set_sub_data(std::size_t offset, std::span<const D> data) const {
        bind();
        glBufferSubData(target_, static_cast<GLintptr>(offset),
                        static_cast<GLsizeiptr>(data.size_bytes()), data.data());
    }

    template <typename S>
    void set_sub_data_unsynchronized(std::size_t offset, std::span<const S> data) const {
        if (data.empty()) {
            return;
        }

        bind();
        void* mapped = glMapBufferRange(
            target_, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(data.size_bytes()),
            GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);

        if (mapped == nullptr) {
            glBufferSubData(target_, static_cast<GLintptr>(offset),
                            static_cast<GLsizeiptr>(data.size_bytes()), data.data());
            return;
        }

        std::memcpy(mapped, data.data(), data.size_bytes());
        glUnmapBuffer(target_);
    }

    template <typename S>
    void orphan_and_set(std::span<const S> data, std::size_t capacity_bytes) const {
        bind();
        glBufferData(target_, static_cast<GLsizeiptr>(capacity_bytes), nullptr, GL_STREAM_DRAW);
        glBufferSubData(target_, 0, static_cast<GLsizeiptr>(data.size_bytes()), data.data());
    }

private:
    GLuint buffer_ = 0;
    GLenum target_ = GL_ARRAY_BUFFER;
};

class VertexArray {
public:
    VertexArray() = default;
    static VertexArray create();
    ~VertexArray();

    VertexArray(const VertexArray&) = delete;
    VertexArray& operator=(const VertexArray&) = delete;
    VertexArray(VertexArray&& other) noexcept;
    VertexArray& operator=(VertexArray&& other) noexcept;

    [[nodiscard]] GLuint id() const { return array_; }

    void bind() const;

    template <typename V>
    void set_attribute(GLenum type_, GLuint attrib_pos, GLint components, GLint offset) const {
        bind();
        if (type_ == GL_FLOAT) {
            glVertexAttribPointer(attrib_pos, components, type_, GL_FALSE,
                                  static_cast<GLsizei>(sizeof(V)),
                                  reinterpret_cast<const void*>(static_cast<std::uintptr_t>(offset)));
        } else {
            glVertexAttribIPointer(attrib_pos, components, type_,
                                   static_cast<GLsizei>(sizeof(V)),
                                   reinterpret_cast<const void*>(static_cast<std::uintptr_t>(offset)));
        }

        glEnableVertexAttribArray(attrib_pos);
    }

private:
    explicit VertexArray(GLuint array) : array_(array) {}

    GLuint array_ = 0;
};

class Texture {
public:
    Texture() = default;
    explicit Texture(GLenum target);
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    void set_wrapping(GLenum wrapping) const;
    void set_filtering(GLenum filter) const;

    void load_raw(std::span<const std::uint8_t> data, int width, int height);

    void update_texture(const std::string& path);
    void update_texture_raw(std::span<const std::uint8_t> data) const;

    void bind() const;

private:
    GLuint tex_ = 0;
    GLenum target_ = GL_TEXTURE_2D;

    int width_ = 1;
    int height_ = 1;
};

#define SET_ATTRIBUTE(GL_TYPE, VAO, POS, STRUCT, FIELD)                        \
    (VAO).set_attribute<STRUCT>((GL_TYPE), static_cast<GLuint>(POS),           \
                                static_cast<GLint>(sizeof(STRUCT::FIELD) / 4), \
                                static_cast<GLint>(offsetof(STRUCT, FIELD)))

}
