#pragma once

#include <memory>
#include <shared_mutex>
#include <vector>

#include "editor/util.h"
#include "midi/events/meta_event.h"
#include "util/shared.h"

namespace andromeda::editor {

struct TempoPoint {
    MIDITick tick = 0;
    float tempo = 0.0f;
    float secs_at_tick = 0.0f;
};

using SharedMetaEvents = util::SharedPtr<std::vector<MetaEvent>>;

class TempoMap {
public:
    TempoMap() : meta_events(util::make_shared_rw<std::vector<MetaEvent>>()) {}

    void rebuild_tempo_map(std::uint16_t ppq);

    [[nodiscard]] float ticks_to_secs_from_map(std::uint16_t ppq, MIDITick tick) const;
    [[nodiscard]] MIDITick secs_to_ticks_from_map(std::uint16_t ppq, float secs) const;
    [[nodiscard]] float get_bpm_at_tick(MIDITick tick) const;

    SharedMetaEvents meta_events;

private:
    std::vector<TempoPoint> tempo_map_;
};

}
