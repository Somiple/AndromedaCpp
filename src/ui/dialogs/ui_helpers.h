#pragma once

#include <imgui.h>
#include <imgui_internal.h>

namespace andromeda::app {

inline void text_sized(float size, const char* text) {
    ImGui::PushFont(nullptr, size);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

inline bool selectable_label(bool selected, const char* text) {
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImGuiStyle& style = ImGui::GetStyle();
    return ImGui::Selectable(text, selected, ImGuiSelectableFlags_None,
                             ImVec2(text_size.x + style.FramePadding.x * 2.0f,
                                    text_size.y + style.FramePadding.y * 2.0f));
}

class DisabledScope {
public:
    explicit DisabledScope(bool enabled) : disabled_(!enabled) {
        if (disabled_) {
            ImGui::BeginDisabled();
        }
    }
    ~DisabledScope() {
        if (disabled_) {
            ImGui::EndDisabled();
        }
    }

    DisabledScope(const DisabledScope&) = delete;
    DisabledScope& operator=(const DisabledScope&) = delete;

private:
    bool disabled_;
};

inline void vertical_separator() { ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); }

}
