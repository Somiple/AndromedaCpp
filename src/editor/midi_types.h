#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace andromeda::editor {

using MIDITick = std::uint32_t;
using SignedMIDITick = std::int32_t;
using MIDITrk = std::uint16_t;
using SignedMIDITrk = std::int16_t;
using MIDIKey = std::uint8_t;
using SignedMIDIKey = std::int8_t;

using MIDITrkVec = std::pair<MIDITick, MIDITrk>;
using SignedMIDITrkVec = std::pair<SignedMIDITick, SignedMIDITrk>;
using MIDIVec = std::pair<MIDITick, MIDIKey>;
using SignedMIDIVec = std::pair<SignedMIDITick, SignedMIDIKey>;

using MIDITickAtomic = std::atomic<MIDITick>;

}
