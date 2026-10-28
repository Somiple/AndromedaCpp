#pragma once

#include <string>
#include <unordered_map>

// `ImageHandle` lives in its own header so widgets that only pass handles around
// do not have to include this loader (and its unordered_map member) at all.
#include "ui/util/image_handle.h"

namespace andromeda::app {

/// Owns every texture the UI preloads from `assets/`, keyed by a string name.
class ImageResources {
public:
    ~ImageResources();

    /// Loads `path` and registers it under `id`; existing registrations are replaced.
    void preload_image(const std::string& path, const std::string& id);

    /// Returns the handle registered under `name`, or an invalid handle if absent.
    [[nodiscard]] ImageHandle get_image_handle(const std::string& name) const;

private:
    std::unordered_map<std::string, ImageHandle> handles_;
};

}
