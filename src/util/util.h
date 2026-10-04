#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>

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

}
