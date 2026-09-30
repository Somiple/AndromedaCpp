#include "editor/tempo_map.h"

#include <algorithm>
#include <utility>

namespace andromeda::editor {

void TempoMap::rebuild_tempo_map(std::uint16_t ppq) {
    std::vector<std::pair<MIDITick, float>> tempos;

    {
        std::shared_lock lock(meta_events->mutex);
        for (const MetaEvent& m : meta_events->value) {
            if (m.event_type == MetaEventType::Tempo) {
                tempos.emplace_back(m.tick, bytes_as_tempo(m.data));
            }
        }
    }

    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    if (tempos.empty() || tempos.front().first != 0) {
        tempos.insert(tempos.begin(), {0, 120.0f});
    }

    tempo_map_.clear();
    float secs = 0.0f;
    for (std::size_t i = 0; i < tempos.size(); ++i) {
        const auto [tick, tempo] = tempos[i];
        if (i > 0) {
            const auto [prev_tick, prev_tempo] = tempos[i - 1];
            const float delta_ticks = static_cast<float>(tick) - static_cast<float>(prev_tick);
            secs += delta_ticks * 60.0f / (prev_tempo * static_cast<float>(ppq));
        }

        tempo_map_.push_back(TempoPoint{.tick = tick, .tempo = tempo, .secs_at_tick = secs});
    }
}

float TempoMap::ticks_to_secs_from_map(std::uint16_t ppq, MIDITick tick) const {
    if (tempo_map_.empty()) {
        return static_cast<float>(tick) * 60.0f / (120.0f * static_cast<float>(ppq));
    }

    const auto it = std::partition_point(tempo_map_.begin(), tempo_map_.end(),
                                         [tick](const TempoPoint& t) { return t.tick <= tick; });
    std::size_t idx = static_cast<std::size_t>(it - tempo_map_.begin());
    idx = idx > 0 ? idx - 1 : 0;

    const TempoPoint& p = tempo_map_[idx];
    return p.secs_at_tick +
           static_cast<float>(tick - p.tick) * 60.0f / (p.tempo * static_cast<float>(ppq));
}

MIDITick TempoMap::secs_to_ticks_from_map(std::uint16_t ppq, float secs) const {
    if (tempo_map_.empty()) {
        return static_cast<MIDITick>(secs * (120.0f * static_cast<float>(ppq)) / 60.0f);
    }

    const auto it = std::partition_point(tempo_map_.begin(), tempo_map_.end(),
                                         [secs](const TempoPoint& t) { return t.secs_at_tick <= secs; });
    std::size_t idx = static_cast<std::size_t>(it - tempo_map_.begin());
    idx = idx > 0 ? idx - 1 : 0;

    const TempoPoint& p = tempo_map_[idx];
    const float sec_per_tick = 60.0f / (p.tempo * static_cast<float>(ppq));
    const float delta_secs = secs - p.secs_at_tick;
    const float delta_ticks = delta_secs / sec_per_tick;
    return p.tick + static_cast<MIDITick>(delta_ticks);
}

}
