#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/rendering/buffers.h"
#include "app/view_settings.h"

namespace andromeda::app {

using NoteColor = std::array<float, 3>;

inline constexpr std::array<NoteColor, 16> DEFAULT_COLORS = {{
    {1.00f, 0.00f, 0.00f},
    {1.00f, 0.25f, 0.00f},
    {1.00f, 0.50f, 0.00f},
    {1.00f, 0.75f, 0.00f},
    {1.00f, 1.00f, 0.00f},
    {0.75f, 1.00f, 0.00f},
    {0.50f, 1.00f, 0.00f},
    {0.25f, 1.00f, 0.00f},
    {0.00f, 1.00f, 0.00f},
    {0.00f, 1.00f, 0.25f},
    {0.00f, 1.00f, 0.50f},
    {0.00f, 1.00f, 0.75f},
    {0.00f, 1.00f, 1.00f},
    {0.00f, 0.50f, 1.00f},
    {0.50f, 0.00f, 1.00f},
    {0.75f, 0.00f, 1.00f},
}};

class NoteColors {
public:
    NoteColors() = default;

    static NoteColors create();

    void load_from_image(const std::string& path);

    [[nodiscard]] std::size_t get_index(std::size_t trk_chan) const {
        const auto [trk, chn] = decode_track_channel(trk_chan);

        switch (index_type_) {
        case NoteColorIndexing::Channel:      return chn;
        case NoteColorIndexing::Track:        return trk & 0xF;
        case NoteColorIndexing::ChannelTrack: return (trk + chn) & 0xF;
        }
        return chn;
    }

    static std::vector<std::uint8_t> generate_texture_data(
        const std::array<NoteColor, 16>& colors);

    rendering::Texture& get_texture() { return *note_texture_; }

    [[nodiscard]] NoteColorIndexing get_index_type() const { return index_type_; }
    void set_index_type(NoteColorIndexing t) {
        index_type_ = t;
        version_ += 1;
    }

    [[nodiscard]] std::uint64_t version() const { return version_; }

private:
    [[nodiscard]] static std::pair<std::size_t, std::size_t> decode_track_channel(
        std::size_t trk_chan) {
        return {trk_chan >> 4, trk_chan & 0xF};
    }

    NoteColorIndexing index_type_ = NoteColorIndexing::Channel;
    std::uint64_t version_ = 0;
    std::array<NoteColor, 16> colors_ = DEFAULT_COLORS;
    std::optional<rendering::Texture> note_texture_;
};

}
