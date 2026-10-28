#pragma once

// REFACTOR: the row-height constants used to live in an anonymous namespace inside
// main_window.cpp. Once the dock wiring moved to its own translation unit that became two
// definitions of the same values, so they are now declared once here and imported by both
// translation units with `using internal::NAME;` — the call sites keep the plain names.
namespace andromeda::app::internal {

inline constexpr float TOOLBAR_ROW_H = 25.0f;     ///< Height of a standard toolbar row, in imgui units.
inline constexpr float TRANSPORT_ROW_H = 23.0f;   ///< Height of the transport control row, in imgui units.
inline constexpr float SLIDER_ROW_H = 22.0f;      ///< Height of a slider row, in imgui units.
inline constexpr float BAR_NUMBER_ROW_H = 26.0f;  ///< Height of the bar-number ruler row, in imgui units.

} // namespace andromeda::app::internal
