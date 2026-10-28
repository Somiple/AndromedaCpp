#include "ui/panels/panels.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>

#include "ui/app/main_window.h"
#include "ui/widgets/toolbar_widgets.h"
#include "core/system_stats.h"

// ---------------------------------------------------------------------------
// Diagnostics panel group.
//
// Extracted from the former monolithic `app/ui/panels.cpp`. Currently holds the
// process CPU/RAM read-out; future performance panels belong here too.
// ---------------------------------------------------------------------------

namespace andromeda::app {

void draw_panel_process_stats(MainWindow& parent) {
    parent.stats().update();

    const float cpu = parent.stats().cpu_usage;
    const float ram_pers = parent.stats().memory_pers;

    const auto colour_for = [](float pers) {
        if (pers >= 90.0f) {
            return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
        }
        if (pers >= 50.0f) {
            return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
        }
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    };

    ImGui::TextColored(colour_for(cpu), "CPU: %.2f%%", cpu);
    toolbar::toolbar_separator();
    ImGui::TextColored(colour_for(ram_pers), "RAM: %s (%.1f%%)",
                       parent.stats().memory_usage.to_string().c_str(), ram_pers);
}

} // namespace andromeda::app
