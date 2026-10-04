#pragma once

#include <cstdint>
#include <string>

namespace andromeda::app {

enum class VS_PianoRoll_OnionState { NoOnion, ViewPrevious, ViewNext, ViewAll };

inline std::string to_string(VS_PianoRoll_OnionState s) {
    switch (s) {
    case VS_PianoRoll_OnionState::NoOnion:      return "No onion";
    case VS_PianoRoll_OnionState::ViewAll:      return "All tracks";
    case VS_PianoRoll_OnionState::ViewNext:     return "Next track";
    case VS_PianoRoll_OnionState::ViewPrevious: return "Previous track";
    }
    return "";
}

enum class VS_PianoRoll_OnionColoring { GrayedOut, PartialColor, FullColor };

inline std::string to_string(VS_PianoRoll_OnionColoring c) {
    switch (c) {
    case VS_PianoRoll_OnionColoring::FullColor:    return "Full Color";
    case VS_PianoRoll_OnionColoring::PartialColor: return "Partial Color";
    case VS_PianoRoll_OnionColoring::GrayedOut:    return "Grayed Out";
    }
    return "";
}

enum class VS_PianoRoll_DataViewState { Hidden, NoteVelocities, PitchBend };

inline std::string to_string(VS_PianoRoll_DataViewState s) {
    switch (s) {
    case VS_PianoRoll_DataViewState::Hidden:         return "Hidden";
    case VS_PianoRoll_DataViewState::NoteVelocities: return "Velocity";
    case VS_PianoRoll_DataViewState::PitchBend:      return "Pitch Bend";
    }
    return "";
}

enum class NoteColorIndexing { Track, Channel, ChannelTrack };

inline std::string to_string(NoteColorIndexing c) {
    switch (c) {
    case NoteColorIndexing::Track:        return "Track";
    case NoteColorIndexing::Channel:      return "Channel";
    case NoteColorIndexing::ChannelTrack: return "Track & Channel";
    }
    return "";
}

enum class CurrentView { PianoRoll, TrackView };

struct ViewSettings {
    VS_PianoRoll_OnionState pr_onion_state = VS_PianoRoll_OnionState::NoOnion;
    VS_PianoRoll_OnionColoring pr_onion_coloring = VS_PianoRoll_OnionColoring::PartialColor;
    VS_PianoRoll_DataViewState pr_dataview_state = VS_PianoRoll_DataViewState::NoteVelocities;
   
    float pr_dataview_size = 200.0f;
    std::uint16_t pr_curr_track = 0;
    bool pr_autoscroll = true;
    bool show_meta_events = false;

    CurrentView current_view = CurrentView::PianoRoll;
};

}
