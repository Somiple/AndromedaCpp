#include "app/shared.h"

namespace andromeda::app {

NoteColors NoteColors::create() {
    NoteColors colors;

    rendering::Texture note_texture(GL_TEXTURE_2D);

    const std::vector<std::uint8_t> note_data = generate_texture_data(DEFAULT_COLORS);
    note_texture.bind();
    note_texture.set_wrapping(GL_REPEAT);
    note_texture.set_filtering(GL_NEAREST);
    note_texture.load_raw(note_data, 16, 1);

    colors.note_texture_ = std::move(note_texture);
    return colors;
}

void NoteColors::load_from_image(const std::string& path) {
    if (note_texture_.has_value()) {
        note_texture_->update_texture(path);
    }
    version_ += 1;
}

std::vector<std::uint8_t> NoteColors::generate_texture_data(
    const std::array<NoteColor, 16>& colors) {
    std::vector<std::uint8_t> data(16 * 3, 0);

    for (std::size_t i = 0; i < colors.size(); ++i) {
        const std::size_t index = i * 3;
        data[index] = static_cast<std::uint8_t>(colors[i][0] * 255.0f);
        data[index + 1] = static_cast<std::uint8_t>(colors[i][1] * 255.0f);
        data[index + 2] = static_cast<std::uint8_t>(colors[i][2] * 255.0f);
    }

    return data;
}

}
