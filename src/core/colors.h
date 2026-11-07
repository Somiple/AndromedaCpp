#pragma once

#include <array>
#include <cstdint>
#include <algorithm>

namespace andromeda::util {

inline constexpr std::array<std::uint8_t, 3> unpack_color_u8(std::uint32_t col) {
	return {
		static_cast<std::uint8_t>((col & 0xFF0000) >> 16),
		static_cast<std::uint8_t>((col & 0xFF00) >> 8),
		static_cast<std::uint8_t>(col & 0xFF)
	};
}

inline constexpr std::array<float, 3> unpack_color_float(std::uint32_t col) {
	return {
		static_cast<float>((col & 0xFF0000) >> 16) / 255.0f,
		static_cast<float>((col & 0xFF00) >> 8) / 255.0f,
		static_cast<float>(col & 0xFF) / 255.0f
	};
}

inline constexpr std::uint32_t pack_color(std::array<std::uint8_t, 3> color) {
	return (static_cast<uint32_t>(color[0]) << 16) |
		   (static_cast<uint32_t>(color[1]) << 8) |
		   (static_cast<uint32_t>(color[2]));
}

inline constexpr std::uint32_t pack_color(std::array<float, 3> color) {
	std::uint32_t r = static_cast<std::uint32_t>(color[0] * 255.0f);
	std::uint32_t g = static_cast<std::uint32_t>(color[1] * 255.0f);
	std::uint32_t b = static_cast<std::uint32_t>(color[2] * 255.0f);

	return (r << 16) | (g << 8) | b;
}

inline constexpr std::uint32_t mix_color(std::uint32_t x, std::uint32_t y, float fac) {
	const auto x_ = unpack_color_float(x);
	const auto y_ = unpack_color_float(y);
	const std::uint32_t r =
		static_cast<std::uint32_t>(std::clamp(((1.0f - fac) * x_[0] + fac * y_[0]) * 255.0f, 0.0f, 255.0f));
	const std::uint32_t g =
		static_cast<std::uint32_t>(std::clamp(((1.0f - fac) * x_[1] + fac * y_[1]) * 255.0f, 0.0f, 255.0f));
	const std::uint32_t b =
		static_cast<std::uint32_t>(std::clamp(((1.0f - fac) * x_[2] + fac * y_[2]) * 255.0f, 0.0f, 255.0f));
	return (r << 16) | (g << 8) | b;
}

inline constexpr std::uint32_t as_imgui_color(std::uint32_t color) {
	return 0xFF000000 | ((color & 0xFF0000) >> 16) | (color & 0xFF00) | ((color & 0xFF) << 16);
}

constexpr const std::uint32_t BLACK = 0x000000;
constexpr const std::uint32_t WHITE = 0xFFFFFF;

constexpr const std::uint32_t BAR_BASE =       pack_color(std::array<float,3>{ 0.2, 0.2, 0.25 });
constexpr const std::uint32_t BAR_DARK =       mix_color(BAR_BASE, BLACK, 0.1);
constexpr const std::uint32_t BAR_DARKER =     mix_color(BAR_BASE, BLACK, 0.5);
constexpr const std::uint32_t BAR_VERYDARK =   mix_color(BAR_DARKER, BLACK, 0.1);
constexpr const std::uint32_t BAR_VERYDARKER = mix_color(BAR_DARKER, BLACK, 0.5);

}