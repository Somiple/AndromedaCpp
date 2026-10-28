#include <string>
#include <vector>
#include "snap_types.h"
#include "snap_types_utils.h"

namespace andromeda::editor::timeline {

std::string grid_snap_to_string(const TimelineGridSnap& grid_snap) {
	switch (grid_snap) {
    case TimelineGridSnap::Bar:           return "Bar";
    case TimelineGridSnap::Beat:          return "Beat";
    case TimelineGridSnap::HalfBeat:      return "1/2 beat";
    case TimelineGridSnap::QuarterBeat:   return "1/4 beat";
    case TimelineGridSnap::WholeNote:     return "Whole note";
    case TimelineGridSnap::HalfNote:      return "Half note";
    case TimelineGridSnap::QuarterNote:   return "Quarter note";
    case TimelineGridSnap::EighthNote:    return "Eighth note";
    case TimelineGridSnap::SixteenthNote: return "1/16th note";
    case TimelineGridSnap::Note32:        return "1/32nd note";
    case TimelineGridSnap::Note64:        return "1/64th note";
    case TimelineGridSnap::Note128:       return "1/128th note";
    case TimelineGridSnap::None:          return "None";
	}

    return "Unknown";
}

TimelineGridSnap string_to_grid_snap(std::string_view str) {
    if (str == "Bar") return TimelineGridSnap::Bar;
    if (str == "Beat") return TimelineGridSnap::Beat;
    if (str == "1/2 beat") return TimelineGridSnap::HalfBeat;
    if (str == "1/4 beat") return TimelineGridSnap::QuarterBeat;
    if (str == "Whole note") return TimelineGridSnap::WholeNote;
    if (str == "Half note") return TimelineGridSnap::HalfNote;
    if (str == "Quarter note") return TimelineGridSnap::QuarterNote;
    if (str == "Eighth note") return TimelineGridSnap::EighthNote;
    if (str == "1/16th note") return TimelineGridSnap::SixteenthNote;
    if (str == "1/32nd note") return TimelineGridSnap::Note32;
    if (str == "1/64th note") return TimelineGridSnap::Note64;
    if (str == "1/128th note") return TimelineGridSnap::Note128;
    if (str == "None") return TimelineGridSnap::None;

    // fallback
    return TimelineGridSnap::QuarterNote;
}

std::vector<std::string> get_grid_snap_strings(bool include_none) {
    std::vector<std::string> snap_strings{};

    // None is the last entry in the enum, so it's okay
    std::size_t size = static_cast<std::size_t>(TimelineGridSnap::None);
    if (!include_none) size -= 1;

    snap_strings.reserve(size);

    for (std::size_t i = 0; i < size; i++) {
        TimelineGridSnap as_grid = static_cast<TimelineGridSnap>(i);
        snap_strings.push_back(grid_snap_to_string(as_grid));
    }

    return snap_strings;
}

}