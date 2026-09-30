#include "midi/events/meta_event.h"

#include <array>
#include <cstdio>

namespace andromeda::midi {

std::string to_string(MetaEventType type) {
    switch (type) {
    case MetaEventType::TimeSignature: return "Time Signature";
    case MetaEventType::Tempo:         return "Tempo";
    case MetaEventType::KeySignature:  return "Key Signature";
    case MetaEventType::Marker:        return "Marker";
    default:                           return "";
    }
}

std::string MetaEvent::get_value_string() const {
    switch (event_type) {
    case MetaEventType::Marker: {
        return std::string(reinterpret_cast<const char*>(data.data()), data.size());
    }
    case MetaEventType::Tempo: {
        const std::uint32_t tempo = static_cast<std::uint32_t>(data[2]) |
                                    (static_cast<std::uint32_t>(data[1]) << 8) |
                                    (static_cast<std::uint32_t>(data[0]) << 16);
        const float tempof = 60000000.0f / static_cast<float>(tempo);

        std::array<char, 64> buf{};
        std::snprintf(buf.data(), buf.size(), "%.9g", static_cast<double>(tempof));
        return std::string(buf.data());
    }
    default:
        return "N/A";
    }
}

}
