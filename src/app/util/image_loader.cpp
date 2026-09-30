#include "app/util/image_loader.h"

#include <glad/glad.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <format>

#include "util/debugger.h"

namespace andromeda::app {

using util::Debugger;

ImageResources::~ImageResources() {
    for (auto& [id, handle] : handles_) {
        if (handle.texture_id != 0) {
            GLuint tex = handle.texture_id;
            glDeleteTextures(1, &tex);
        }
    }
}

void ImageResources::preload_image(const std::string& path, const std::string& id) {
    if (handles_.contains(id)) {
        Debugger::log_warning(
            std::format("texture with id {} already exists, will overwrite", id));
    }

    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);

    if (pixels == nullptr) {
        Debugger::log_error(std::format("Failed to open {}: {}", path, stbi_failure_reason()));
        return;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(pixels);

    handles_[id] = ImageHandle{tex, static_cast<float>(w), static_cast<float>(h)};
}

ImageHandle ImageResources::get_image_handle(const std::string& name) const {
    const auto it = handles_.find(name);
    return it == handles_.end() ? ImageHandle{} : it->second;
}

}
