#pragma once 

namespace andromeda::editor::timeline {

enum class TimelineGridSnap;

// Returns a given TimelineGridSnap as a string; typically used for UI combobox labels.
std::string grid_snap_to_string(const TimelineGridSnap& grid_snap);
TimelineGridSnap string_to_grid_snap(std::string_view str);
std::vector<std::string> get_grid_snap_strings(bool include_none = true);

}