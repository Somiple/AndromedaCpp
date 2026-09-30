#include "app/rendering/shaders.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "util/debugger.h"

namespace andromeda::app::rendering {

using util::Debugger;

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        const std::string msg = std::format("Failed to open shader '{}'", path.string());
        Debugger::log_error(msg);
        throw std::runtime_error(msg);
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

GLuint compile(GLenum type, const std::string& src, std::string_view what) {
    const GLuint shader = glCreateShader(type);
    const char* src_ptr = src.c_str();
    const auto src_len = static_cast<GLint>(src.size());
    glShaderSource(shader, 1, &src_ptr, &src_len);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_FALSE) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<std::size_t>(len > 0 ? len : 1));
        glGetShaderInfoLog(shader, len, nullptr, log.data());

        const std::string msg = std::format("{} shader error\n{}", what, log.data());
        Debugger::log_error(msg);
        glDeleteShader(shader);
        throw std::runtime_error(msg);
    }

    return shader;
}

}

ShaderProgram ShaderProgram::create_from_files(std::string_view shader_path) {
    const std::filesystem::path base(shader_path);
    const std::string src_vert = read_file(std::filesystem::absolute(base.string() + ".vert"));
    const std::string src_frag = read_file(std::filesystem::absolute(base.string() + ".frag"));

    const GLuint vert = compile(GL_VERTEX_SHADER, src_vert, "Vertex");
    GLuint frag = 0;
    try {
        frag = compile(GL_FRAGMENT_SHADER, src_frag, "Fragment");
    } catch (...) {
        glDeleteShader(vert);
        throw;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        GLint len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(static_cast<std::size_t>(len > 0 ? len : 1));
        glGetProgramInfoLog(program, len, nullptr, log.data());

        const std::string msg = std::format("Program link error\n{}", log.data());
        Debugger::log_error(msg);
        glDeleteShader(vert);
        glDeleteShader(frag);
        glDeleteProgram(program);
        throw std::runtime_error(msg);
    }

    Debugger::log("Program linked.");
    glDeleteShader(vert);
    glDeleteShader(frag);

    return ShaderProgram(program);
}

ShaderProgram::~ShaderProgram() {
    if (program_ != 0) {
        glDeleteProgram(program_);
    }
}

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept
    : program_(other.program_), uniforms_(std::move(other.uniforms_)) {
    other.program_ = 0;
    other.uniforms_.clear();
}

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
    if (this != &other) {
        if (program_ != 0) {
            glDeleteProgram(program_);
        }
        program_ = other.program_;
        uniforms_ = std::move(other.uniforms_);
        other.program_ = 0;
        other.uniforms_.clear();
    }
    return *this;
}

GLint ShaderProgram::location(const char* name) const {
    for (const CachedUniform& u : uniforms_) {
        if (u.key == name) {
            return u.location;
        }
    }
    for (CachedUniform& u : uniforms_) {
        if (u.name == name) {
            u.key = name;
            return u.location;
        }
    }

    const GLint loc = glGetUniformLocation(program_, name);
    uniforms_.push_back(CachedUniform{name, std::string(name), loc});
    return loc;
}

GLint ShaderProgram::get_attrib_location(const char* attrib) const {
    return glGetAttribLocation(program_, attrib);
}

void ShaderProgram::set_float(const char* name, float value) const {
    glUniform1f(location(name), value);
}

void ShaderProgram::set_int(const char* name, int value) const {
    glUniform1i(location(name), value);
}

void ShaderProgram::set_uint(const char* name, std::uint32_t value) const {
    glUniform1ui(location(name), value);
}

void ShaderProgram::set_vec3_array(const char* name, const float* values, int count) const {
    glUniform3fv(glGetUniformLocation(program_, name), count, values);
}

}
