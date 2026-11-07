#pragma once

// ---------------------------------------------------------------------------
// Layer: core
// Dependency direction:  core -> midi -> editor -> audio -> ui
// This layer includes nothing from the project; every other layer may include
// `core/...`. Keep it free of ImGui, GLFW, GL and audio/MIDI types.
// See docs/ARCHITECTURE.md.
// ---------------------------------------------------------------------------

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>
#include <variant>
#include <vector>

#define loop for (;;)

namespace andromeda::util {

std::expected<void, std::string> send_discord_webhook_crash_message(
    std::string_view webhook_url,
    std::string_view content,
    std::string_view api_key,
    const std::optional<std::string>& reporter_name,
    const std::optional<std::string>& report_details);

// fixed rust bug: b_min and b_max were ignored, so it only normalised to 0..1
template <typename T>
constexpr T remap_range(T val, T a_min, T a_max, T b_min, T b_max) {
    return b_min + (val - a_min) / (a_max - a_min) * (b_max - b_min);
}

std::string format_duration(double secs);

constexpr const char* KEY_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

std::string get_key_name(std::uint8_t key);

const std::uint32_t bpm_to_us(double bpm);
const double us_to_bpm(std::uint32_t us);
const std::vector<std::uint8_t> bpm_to_bytes(double bpm);
const double bytes_to_bpm(std::span<const std::uint8_t> bytes);

template <typename T, typename Var>
inline constexpr bool is_type(const Var&& v) noexcept {
    return std::holds_alternative<T>(v);
}

}
