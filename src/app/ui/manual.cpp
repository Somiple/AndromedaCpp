#include "app/ui/manual.h"

#include <imgui.h>

#include "app/ui/ui_helpers.h"

namespace andromeda::app {

void EditorManualDialog::draw_welcome_tab() const {
    text_sized(20.0f, "Welcome to Andromeda.");
    ImGui::TextWrapped(
        "Welcome to andromeda, the most well-optimized MIDI editor there is out there.");
}

void EditorManualDialog::draw_navigating_tab() const {
    text_sized(20.0f, "Navigating");
    ImGui::TextWrapped(
        "If you are familiar with any MIDI editor such as FL Studio or Domino, navigation in "
        "Andromeda is pretty straightforward, however, do note that some things in this editor "
        "differ.");

    const auto underlined = [](const char* text) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::CalcTextSize(text);
        ImGui::TextUnformatted(text);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, pos.y + size.y),
                                            ImVec2(pos.x + size.x, pos.y + size.y),
                                            ImGui::GetColorU32(ImGuiCol_Text));
    };

    ImGui::TextUnformatted("To switch between the ");
    ImGui::SameLine(0.0f, 0.0f);
    underlined("Piano Roll");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(" and the ");
    ImGui::SameLine(0.0f, 0.0f);
    underlined("Track View");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(", press ");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted("[TAB]");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(".");
}

MaybeDlgAction EditorManualDialog::draw(const ImageResources&) {
    ImGui::BeginGroup();
    if (selectable_label(manual_section_ == Section::Welcome, "Welcome")) {
        manual_section_ = Section::Welcome;
    }
    if (selectable_label(manual_section_ == Section::Navigating, "Navigating")) {
        manual_section_ = Section::Navigating;
    }
    ImGui::EndGroup();

    ImGui::SameLine();
    vertical_separator();
    ImGui::SameLine();

    ImGui::BeginChild("##manual_body", ImVec2(520.0f, 260.0f), ImGuiChildFlags_None);
    switch (manual_section_) {
    case Section::Welcome:
        draw_welcome_tab();
        break;
    case Section::Navigating:
        draw_navigating_tab();
        break;
    }
    ImGui::EndChild();

    return std::nullopt;
}

}
