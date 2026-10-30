#pragma once

// ---------------------------------------------------------------------------
// Layer: editor
// One shared vocabulary for the mouse-selection modifiers, so the piano roll and
// the track view cannot disagree about what shift and alt mean.
// No ImGui here on purpose: the widgets publish the modifier booleans.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace andromeda::editor {

/// Which selection verb a mouse gesture performs.
/// Precedence is deliberate: subtract wins over add, add wins over replace, so a stray
/// combination of modifiers can never silently replace an existing selection.
enum class SelectionGesture : std::uint8_t {
    Replace,   ///< plain click: the new region becomes the whole selection
    Add,       ///< shift: the new region is added to the existing selection
    Subtract   ///< alt: the swept region is removed from the existing selection
};

/// @param shift true when the additive modifier is held at gesture start.
/// @param alt true when the subtractive modifier is held at gesture start.
/// @return the selection verb those modifiers describe.
[[nodiscard]] constexpr SelectionGesture selection_gesture_of(bool shift, bool alt) noexcept {
    if (alt) {
        return SelectionGesture::Subtract;
    }
    if (shift) {
        return SelectionGesture::Add;
    }
    return SelectionGesture::Replace;
}

} // namespace andromeda::editor
