#include "app/rendering/buffers.h"

#include <format>

#include <stb_image.h>

#include "util/debugger.h"

namespace andromeda::app::rendering {

using util::Debugger;

Buffer::Buffer(GLenum target) : target_(target) { glGenBuffers(1, &buffer_); }

Buffer::~Buffer() {
    if (buffer_ != 0) {
        glDeleteBuffers(1, &buffer_);
    }
}

Buffer::Buffer(Buffer&& other) noexcept : buffer_(other.buffer_), target_(other.target_) {
    other.buffer_ = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        if (buffer_ != 0) {
            glDeleteBuffers(1, &buffer_);
        }
        buffer_ = other.buffer_;
        target_ = other.target_;
        other.buffer_ = 0;
    }
    return *this;
}

void Buffer::bind() const { glBindBuffer(target_, buffer_); }

VertexArray VertexArray::create() {
    GLuint array = 0;
    glGenVertexArrays(1, &array);
    return VertexArray(array);
}

VertexArray::~VertexArray() {
    if (array_ != 0) {
        glDeleteVertexArrays(1, &array_);
    }
}

VertexArray::VertexArray(VertexArray&& other) noexcept : array_(other.array_) {
    other.array_ = 0;
}

VertexArray& VertexArray::operator=(VertexArray&& other) noexcept {
    if (this != &other) {
        if (array_ != 0) {
            glDeleteVertexArrays(1, &array_);
        }
        array_ = other.array_;
        other.array_ = 0;
    }
    return *this;
}

void VertexArray::bind() const { glBindVertexArray(array_); }

Texture::Texture(GLenum target) : target_(target) { glGenTextures(1, &tex_); }

Texture::~Texture() {
    if (tex_ != 0) {
        glDeleteTextures(1, &tex_);
    }
}

Texture::Texture(Texture&& other) noexcept
    : tex_(other.tex_), target_(other.target_), width_(other.width_), height_(other.height_) {
    other.tex_ = 0;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (tex_ != 0) {
            glDeleteTextures(1, &tex_);
        }
        tex_ = other.tex_;
        target_ = other.target_;
        width_ = other.width_;
        height_ = other.height_;
        other.tex_ = 0;
    }
    return *this;
}

void Texture::set_wrapping(GLenum wrapping) const {
    glTexParameteri(target_, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrapping));
    glTexParameteri(target_, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrapping));
}

void Texture::set_filtering(GLenum filter) const {
    glTexParameteri(target_, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
    glTexParameteri(target_, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
}

void Texture::load_raw(std::span<const std::uint8_t> data, int width, int height) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(target_, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data.data());
    glGenerateMipmap(target_);

    width_ = width;
    height_ = height;
}

void Texture::update_texture(const std::string& path) {
    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 3);

    if (pixels == nullptr) {
        Debugger::log_error(std::format("Failed to open {}: {}", path, stbi_failure_reason()));
        return;
    }

    update_texture_raw(std::span<const std::uint8_t>(
        pixels, static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3));

    stbi_image_free(pixels);
}

void Texture::update_texture_raw(std::span<const std::uint8_t> data) const {
    if (data.size() != static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 3) {
        Debugger::log_error(std::format(
            "Texture update size mismatch: got {} bytes, expected {}", data.size(),
            static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 3));
        return;
    }

    bind();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(target_, 0, 0, 0, width_, height_, GL_RGB, GL_UNSIGNED_BYTE, data.data());
}

void Texture::bind() const { glBindTexture(target_, tex_); }

}
