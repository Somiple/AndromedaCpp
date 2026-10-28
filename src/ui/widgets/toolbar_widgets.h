#pragma once

#include <imgui.h>

// Only the handle type is needed here: panels pass ImageHandles in, they do not
// own the loader, so this header must not pull in `ImageResources`.
#include "ui/util/image_handle.h"

// ---------------------------------------------------------------------------
// Reusable toolbar widgets.
//
// Extracted from `app/ui/panels.cpp`, where these helpers lived in an anonymous
// namespace. Every toolbar-style panel (editor tools, playback, diagnostics)
// needs the same icon buttons, checkboxes, sliders and number fields, so they
// belong in their own module instead of being duplicated per panel file.
// Behaviour is byte-for-byte identical to the previous local implementations.
// ---------------------------------------------------------------------------

namespace andromeda::app::toolbar {

/// Default square size used by toolbar icon buttons.
inline constexpr float TOOLBAR_ICON_SIZE = 20.0f;

/// Size of the custom-drawn checkbox used by the playback panel.
inline constexpr float EGUI_CHECKBOX_SIZE = 14.0f;

/// Vertically centres the next row of widgets against a row of `row_height`.
void center_row_against(float row_height);

/// Height of a standard toolbar row, derived from the icon size and frame padding.
float toolbar_row_height();

/// Vertical separator that participates in a horizontal (`SameLine`) row.
///
/// Named `toolbar_separator` rather than `vertical_separator` to avoid clashing
/// with the unrelated inline helper of that name in `ui/dialogs/ui_helpers.h`.
void toolbar_separator();

/// Image button with a text fallback, optional selection state and disabled look.
bool icon_button(const char* id, const ImageHandle& img, bool selected, bool enabled,
                 const char* fallback_label, float size = 0.0f, bool frame = true);

/// Small custom-drawn checkbox matching the egui visual style.
bool egui_checkbox(const char* id, bool* value);

/// Custom-drawn integer slider; returns true when the value changed this frame.
bool egui_slider_int(const char* id, int* value, int v_min, int v_max, float width);

/// Labelled numeric text field clamped to [min_value, max_value].
bool number_field(const char* label, int* value, int min_value, int max_value, float width);

} // namespace andromeda::app::toolbar

// Convenience wrapper for a single centred toolbar row.
// The identifiers are fully qualified so the macro stays valid in any namespace.
#define CENTERED(...)                                                                    \
    do {                                                                                 \
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));              \
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));              \
        ::andromeda::app::toolbar::center_row_against(                                   \
            ::andromeda::app::toolbar::toolbar_row_height());                            \
        { __VA_ARGS__ }                                                                  \
        ImGui::PopStyleVar(2);                                                           \
    } while (0)
