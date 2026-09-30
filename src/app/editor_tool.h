#pragma once

#include <cstdint>
#include <utility>

namespace andromeda::app {

enum class EditorTool {
    Pencil,
    Eraser,
    Selector,
};

struct EditorToolSettings {
    EditorTool curr_tool = EditorTool::Pencil;
    std::pair<std::uint8_t, std::uint16_t> snap_ratio{1, 4};
};

struct ToolBarSettings {
    int note_gate = 960;
    int note_velocity = 100;
    int note_channel = 1;
};

}
