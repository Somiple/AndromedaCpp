#pragma once

#include <vector>

#include "midi/events/channel_event.h"

namespace andromeda::midi {

struct MIDIEvent;

std::vector<MIDIEvent> channel_to_midi_ev(const std::vector<ChannelEvent>& channel_evs);

std::vector<MIDIEvent> merge_events(std::vector<MIDIEvent> seq1, std::vector<MIDIEvent> seq2);

}
