#include "editor/midi_bar_cacher.h"

namespace andromeda::editor {

std::pair<std::uint32_t, std::uint32_t> BarCacher::get_bar_interval(std::size_t bar_num) {
    if (bar_num < bar_cache.size()) {
        return bar_cache[bar_num];
    }

    validate_bars_until(bar_num);

    return bar_cache[bar_num];
}

void BarCacher::validate_bars_until(std::size_t target_bar) {
    std::shared_lock pm_lock(project_manager->mutex);
    const ProjectManager& pm = project_manager->value;

    std::shared_lock meta_lock(pm.get_metas()->mutex);
    const std::vector<MetaEvent>& metas = pm.get_metas()->value;

    while (bar_cache.size() <= target_bar) {
        const std::uint32_t start_tick =
            bar_cache.empty() ? 0u : bar_cache.back().first + bar_cache.back().second;

        const auto [length, new_idx] =
            compute_bar_length_at(start_tick, metas, last_ts_index_, pm.get_ppq());
        last_ts_index_ = new_idx;
        bar_cache.emplace_back(start_tick, length);
    }
}

std::pair<std::uint32_t, std::size_t> BarCacher::compute_bar_length_at(
    std::uint32_t start_tick, const std::vector<MetaEvent>& metas, std::size_t search_idx,
    std::uint16_t ppq) const {
    const MetaEvent* current_ts = nullptr;

    // fixed rust bug: the cached index never advanced, so every bar rescanned from 0
    if (search_idx > metas.size()) {
        search_idx = 0;
    }
    std::size_t last_idx = search_idx;

    for (std::size_t i = search_idx; i < metas.size(); ++i) {
        if (metas[i].event_type == MetaEventType::TimeSignature) {
            if (metas[i].tick <= start_tick) {
                current_ts = &metas[i];
                last_idx = i;
            } else {
                break;
            }
        }
    }

    std::uint32_t num = 4;
    std::uint32_t den = 2;
    if (current_ts != nullptr) {
        num = static_cast<std::uint32_t>(current_ts->data[0]);
        den = static_cast<std::uint32_t>(current_ts->data[1]);
    }

    const std::uint32_t ticks_per_beat = static_cast<std::uint32_t>(ppq) << 2;
    const std::uint32_t nominal = (num * ticks_per_beat) >> den;

    const MetaEvent* next_ts = nullptr;
    for (std::size_t i = last_idx; i < metas.size(); ++i) {
        if (metas[i].event_type == MetaEventType::TimeSignature && metas[i].tick > start_tick) {
            next_ts = &metas[i];
            break;
        }
    }

    std::uint32_t length = nominal;
    if (next_ts != nullptr) {
        const std::uint32_t next_tick = next_ts->tick;
        if (next_tick < start_tick + nominal) {
            length = next_tick - start_tick;
        }
    }

    return {length, last_idx};
}

}
