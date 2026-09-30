#include "app/rendering/screen_coverage.h"

#include <algorithm>
#include <cmath>

namespace andromeda::app::rendering {

namespace {

constexpr std::uint64_t ALL_ONES = ~0ull;

constexpr std::uint64_t word_mask(unsigned lo, unsigned hi) {
    const std::uint64_t high = hi >= 64u ? ALL_ONES : ((1ull << hi) - 1ull);
    const std::uint64_t low = lo == 0u ? ALL_ONES : ~((1ull << lo) - 1ull);
    return high & low;
}

bool all_set(const std::uint64_t* row, std::size_t lo, std::size_t hi) {
    const std::size_t first_w = lo >> 6;
    const std::size_t last_w = (hi - 1) >> 6;

    if (first_w == last_w) {
        const std::uint64_t m =
            word_mask(static_cast<unsigned>(lo & 63u), static_cast<unsigned>(hi - (first_w << 6)));
        return (row[first_w] & m) == m;
    }

    std::uint64_t m = word_mask(static_cast<unsigned>(lo & 63u), 64u);
    if ((row[first_w] & m) != m) {
        return false;
    }
    for (std::size_t w = first_w + 1; w < last_w; ++w) {
        if (row[w] != ALL_ONES) {
            return false;
        }
    }
    m = word_mask(0u, static_cast<unsigned>(hi - (last_w << 6)));
    return (row[last_w] & m) == m;
}

void set_bits(std::uint64_t* row, std::size_t lo, std::size_t hi) {
    const std::size_t first_w = lo >> 6;
    const std::size_t last_w = (hi - 1) >> 6;

    if (first_w == last_w) {
        row[first_w] |=
            word_mask(static_cast<unsigned>(lo & 63u), static_cast<unsigned>(hi - (first_w << 6)));
        return;
    }

    row[first_w] |= word_mask(static_cast<unsigned>(lo & 63u), 64u);
    for (std::size_t w = first_w + 1; w < last_w; ++w) {
        row[w] = ALL_ONES;
    }
    row[last_w] |= word_mask(0u, static_cast<unsigned>(hi - (last_w << 6)));
}

std::size_t clamp_column(double x, std::size_t columns) {
    if (!(x > 0.0)) {
        return 0;
    }
    if (x >= static_cast<double>(columns)) {
        return columns;
    }
    return static_cast<std::size_t>(x);
}

}

void ScreenCoverage::begin_frame(float tick_pos, float zoom_ticks, float key_pos, float zoom_keys,
                                 float note_area_px) {
    active_ = false;
    dirty_rows_.clear();
    row_pending_.fill(false);
    selected_.clear();
    all_selected_ = true;
    tested_ = 0;
    skipped_ = 0;

    if (!enabled_ || !(zoom_ticks > 0.0f) || !(zoom_keys > 0.0f) || !(note_area_px >= 1.0f) ||
        note_area_px > static_cast<float>(MAX_COLUMNS)) {
        return;
    }

    columns_ = static_cast<std::size_t>(std::ceil(static_cast<double>(note_area_px)));
    words_ = (columns_ + 63) / 64;
    scale_ = static_cast<double>(note_area_px) / static_cast<double>(zoom_ticks);
    tick_pos_ = static_cast<double>(tick_pos);

    const std::size_t need = ROWS * words_;
    if (covered_.size() != need) {
        covered_.assign(need, 0ull);
        pending_.assign(need, 0ull);
    } else {
        std::fill(covered_.begin(), covered_.end(), 0ull);
        std::fill(pending_.begin(), pending_.end(), 0ull);
    }

    for (std::size_t k = 0; k < ROWS; ++k) {
        const float key = static_cast<float>(k);
        row_visible_[k] = !(key + 1.0f < key_pos || key > key_pos + zoom_keys);
    }

    active_ = true;
}

void ScreenCoverage::select(const std::vector<midi::Note>& notes, std::size_t first,
                            std::size_t end, bool cull) {
    selected_.clear();
    all_selected_ = true;

    if (!active_ || end <= first || end > notes.size()) {
        return;
    }

    selected_.reserve(end - first);
    tested_ += end - first;

    const auto columns_f = static_cast<double>(columns_);

    for (std::size_t i = first; i < end; ++i) {
        const midi::Note& note = notes[i];
        const std::size_t row = note.key;

        if (!row_visible_[row]) {
            all_selected_ = false;
            skipped_ += 1;
            continue;
        }

        const double x0 = (static_cast<double>(note.start) - tick_pos_) * scale_;
        const double x1 =
            (static_cast<double>(note.start) + static_cast<double>(note.length) - tick_pos_) *
            scale_;

        if (x1 <= -1.0 || x0 >= columns_f + 1.0) {
            all_selected_ = false;
            skipped_ += 1;
            continue;
        }

        const std::size_t touch_lo = clamp_column(std::floor(x0), columns_);
        const std::size_t touch_hi = clamp_column(std::ceil(x1), columns_);

        if (cull && touch_hi > touch_lo && all_set(&covered_[row * words_], touch_lo, touch_hi)) {
            all_selected_ = false;
            skipped_ += 1;
            continue;
        }

        selected_.push_back(static_cast<std::uint32_t>(i));

        const std::size_t mark_lo = clamp_column(std::ceil(x0), columns_);
        const std::size_t mark_hi = clamp_column(std::floor(x1), columns_);
        if (mark_hi > mark_lo) {
            set_bits(&pending_[row * words_], mark_lo, mark_hi);
            if (!row_pending_[row]) {
                row_pending_[row] = true;
                dirty_rows_.push_back(static_cast<std::uint8_t>(row));
            }
        }
    }
}

void ScreenCoverage::flush_track() {
    for (const std::uint8_t row : dirty_rows_) {
        std::uint64_t* dst = &covered_[static_cast<std::size_t>(row) * words_];
        std::uint64_t* src = &pending_[static_cast<std::size_t>(row) * words_];
        for (std::size_t w = 0; w < words_; ++w) {
            dst[w] |= src[w];
            src[w] = 0ull;
        }
        row_pending_[row] = false;
    }
    dirty_rows_.clear();
}

}
