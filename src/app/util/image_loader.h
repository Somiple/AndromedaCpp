#pragma once

#include <string>
#include <unordered_map>

namespace andromeda::app {

struct ImageHandle {
    unsigned int texture_id = 0;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] bool valid() const { return texture_id != 0; }
};

class ImageResources {
public:
    ~ImageResources();

    void preload_image(const std::string& path, const std::string& id);

    [[nodiscard]] ImageHandle get_image_handle(const std::string& name) const;

private:
    std::unordered_map<std::string, ImageHandle> handles_;
};

}
