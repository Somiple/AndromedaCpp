#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "midi/events/note.h"

// assumes notes are opaque and fill their whole rect; recheck if shaders change

namespace andromeda::app::rendering {

class ScreenCoverage {
public:
    static void set_enabled(bool on) { enabled_ = on; }
    [[nodiscard]] static bool enabled() { return enabled_; }

    void begin_frame(float tick_pos, float zoom_ticks, float key_pos, float zoom_keys,
                     float note_area_px);

    [[nodiscard]] bool active() const { return active_; }

    [[nodiscard]] static bool worth_running(std::size_t range) {
        return enabled_ && range <= MAX_RANGE;
    }

    void select(const std::vector<midi::Note>& notes, std::size_t first, std::size_t end,
                bool cull);

    void mark_range(const std::vector<midi::Note>& notes, std::size_t first, std::size_t end) {
        if (!worth_running(end - first)) {
            return;
        }
        select(notes, first, end, false);
    }

    // call once per track, whichever path drew it
    void flush_track();

    [[nodiscard]] const std::vector<std::uint32_t>& selected() const { return selected_; }
    [[nodiscard]] bool all_selected() const { return all_selected_; }

    [[nodiscard]] std::size_t tested() const { return tested_; }
    [[nodiscard]] std::size_t skipped() const { return skipped_; }

private:
    // the shader reads key as raw.z & 255 and files carry keys above 127
    static constexpr std::size_t ROWS = 256;

    static constexpr std::size_t MAX_COLUMNS = 16384;

    static constexpr std::size_t MAX_RANGE = 4096;

    inline static bool enabled_ = false;

    bool active_ = false;
    double scale_ = 0.0;
    double tick_pos_ = 0.0;
    std::size_t columns_ = 0;
    std::size_t words_ = 0;

    std::vector<std::uint64_t> covered_;
    std::vector<std::uint64_t> pending_;

    std::array<bool, ROWS> row_visible_{};
    std::array<bool, ROWS> row_pending_{};
    std::vector<std::uint8_t> dirty_rows_;

    std::vector<std::uint32_t> selected_;
    bool all_selected_ = true;

    std::size_t tested_ = 0;
    std::size_t skipped_ = 0;
};

}
