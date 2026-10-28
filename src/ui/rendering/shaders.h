#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glad/glad.h>

namespace andromeda::app::rendering {

class ShaderProgram {
public:
    static ShaderProgram create_from_files(std::string_view shader_path);

    ShaderProgram() = default;
    ~ShaderProgram();

    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;
    ShaderProgram(ShaderProgram&& other) noexcept;
    ShaderProgram& operator=(ShaderProgram&& other) noexcept;

    [[nodiscard]] GLuint id() const { return program_; }

    [[nodiscard]] GLint get_attrib_location(const char* attrib) const;

    void set_float(const char* name, float value) const;
    void set_int(const char* name, int value) const;
    void set_uint(const char* name, std::uint32_t value) const;
    void set_vec3_array(const char* name, const float* values, int count) const;

private:
    explicit ShaderProgram(GLuint program) : program_(program) {}

    struct CachedUniform {
        const char* key = nullptr;
        std::string name;
        GLint location = -1;
    };

    [[nodiscard]] GLint location(const char* name) const;

    GLuint program_ = 0;
    mutable std::vector<CachedUniform> uniforms_;
};

}
