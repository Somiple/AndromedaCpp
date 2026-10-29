#include "ui/dialogs/manual.h"

#include <imgui.h>

#include "ui/dialogs/ui_helpers.h"

namespace andromeda::app {

void EditorManualDialog::draw_welcome_tab() const {
    text_sized(24.0f, "Welcome to Andromeda.");
    ImGui::Spacing();
    ImGui::TextWrapped("Andromeda is a high-performance MIDI editor designed"
        "to make editing MIDI files fast and trivial.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    text_sized(18.0f, "Getting started");

    ImGui::TextWrapped(
        "If this is your first time using Andromeda, don't worry! "
        "This manual will walk you through the essentials, from "
        "finding your way around the editor to creating and editing "
        "your first MIDI notes."
    );

    ImGui::Spacing();

    text_sized(16.0f, "What you'll learn");

    ImGui::BulletText("Navigate the Piano Roll and Track View.");
    ImGui::BulletText("Create, select, move, and resize notes.");
    ImGui::BulletText("Understand the timeline and snapping.");
    ImGui::BulletText("Play back, save, and manage your project.");
    ImGui::BulletText("Undo mistakes and discover useful shortcuts.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    text_sized(16.0f, "A quick tip");

    ImGui::TextWrapped("Don't feel pressured to learn everything at once. "
        "Start with what you are unfamiliar with, then explore the more advanced"
        " stuff whenever you are ready.");
}

void EditorManualDialog::draw_navigating_tab() const {
    text_sized(24.0f, "Getting Around");

    ImGui::Spacing();

    ImGui::TextWrapped(
        "Andromeda has different views for working with your MIDI "
        "project. The two you'll want to become familiar with first "
        "are the Piano Roll and Track View."
    );

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    text_sized(18.0f, "Piano Roll");

    ImGui::TextWrapped(
        "The Piano Roll is where you work directly with individual "
        "MIDI notes. It lets you see their pitch and position on the "
        "timeline, making it the main workspace for note editing."
    );

    ImGui::Spacing();

    text_sized(18.0f, "Track View");

    ImGui::TextWrapped(
        "Track View provides a different perspective on your project, "
        "helping you work with its tracks and overall arrangement."
    );

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    text_sized(18.0f, "Switching views");

    ImGui::TextWrapped(
        "Press TAB to switch between Piano Roll and Track View."
    );

    ImGui::Spacing();

    ImGui::BulletText(
        "Use Piano Roll when working on individual notes."
    );

    ImGui::BulletText(
        "Use Track View when working with the broader project."
    );
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
