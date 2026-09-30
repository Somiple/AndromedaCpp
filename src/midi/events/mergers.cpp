#include "midi/events/mergers.h"

#include <utility>
#include <variant>

#include "midi/midi_file.h"

namespace andromeda::midi {

std::vector<MIDIEvent> channel_to_midi_ev(const std::vector<ChannelEvent>& channel_evs) {
    MIDITick last_time = 0;

    std::vector<MIDIEvent> out;
    out.reserve(channel_evs.size());

    for (const ChannelEvent& ch_ev : channel_evs) {
        const MIDITick curr_time = ch_ev.tick;
        const MIDITick delta = curr_time - last_time;
        last_time = curr_time;

        const std::uint8_t channel = ch_ev.channel;

        std::vector<std::uint8_t> data = std::visit(
            [channel](const auto& ev) -> std::vector<std::uint8_t> {
                using T = std::decay_t<decltype(ev)>;

                if constexpr (std::is_same_v<T, NoteOff>) {
                    return {static_cast<std::uint8_t>(0x80 | channel), ev.key, 0x00};
                } else if constexpr (std::is_same_v<T, NoteOn>) {
                    return {static_cast<std::uint8_t>(0x90 | channel), ev.key, ev.velocity};
                } else if constexpr (std::is_same_v<T, NoteAftertouch>) {
                    return {static_cast<std::uint8_t>(0xA0 | channel), ev.key, ev.pressure};
                } else if constexpr (std::is_same_v<T, Controller>) {
                    return {static_cast<std::uint8_t>(0xB0 | channel), ev.controller, ev.value};
                } else if constexpr (std::is_same_v<T, ProgramChange>) {
                    return {static_cast<std::uint8_t>(0xC0 | channel), ev.program};
                } else if constexpr (std::is_same_v<T, ChannelAftertouch>) {
                    return {static_cast<std::uint8_t>(0xD0 | channel), ev.amount};
                } else {
                    static_assert(std::is_same_v<T, PitchBend>);
                    return {static_cast<std::uint8_t>(0xE0 | channel), ev.lsb, ev.msb};
                }
            },
            ch_ev.event_type);

        out.push_back(MIDIEvent{.delta = delta, .data = std::move(data)});
    }

    return out;
}

std::vector<MIDIEvent> merge_events(std::vector<MIDIEvent> seq1, std::vector<MIDIEvent> seq2) {
    std::vector<MIDIEvent> res;
    res.reserve(seq1.size() + seq2.size());

    std::size_t i1 = 0;
    std::size_t i2 = 0;

    while (i1 < seq1.size() || i2 < seq2.size()) {
        if (i1 < seq1.size() && i2 < seq2.size()) {
            if (seq1[i1].delta <= seq2[i2].delta) {
                seq2[i2].delta -= seq1[i1].delta;
                res.push_back(std::move(seq1[i1++]));
            } else {
                seq1[i1].delta -= seq2[i2].delta;
                res.push_back(std::move(seq2[i2++]));
            }
        } else if (i1 < seq1.size()) {
            res.push_back(std::move(seq1[i1++]));
        } else {
            res.push_back(std::move(seq2[i2++]));
        }
    }

    return res;
}

}
