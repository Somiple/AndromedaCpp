#pragma once

namespace andromeda::app {

/// A loaded texture together with its pixel dimensions.
///
/// Lives in its own header (rather than `image_loader.h`) so that consumers which
/// only *pass around* a handle - toolbar widgets, panels - do not pull in the
/// `std::unordered_map`-owning `ImageResources` loader as well.
struct ImageHandle {
    unsigned int texture_id = 0;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] bool valid() const { return texture_id != 0; }
};

} // namespace andromeda::app
