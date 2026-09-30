#pragma once

#include <cmath>
#include <limits>
#include <type_traits>

namespace andromeda::util {

template <typename T>
[[nodiscard]] constexpr T saturating_cast(float v) noexcept {
    static_assert(std::is_integral_v<T>, "saturating_cast is for integer targets");

    // negated compare on purpose: also catches nan
    if (!(v > static_cast<float>(std::numeric_limits<T>::lowest()))) {
        return std::numeric_limits<T>::lowest();
    }
    if (v >= static_cast<float>(std::numeric_limits<T>::max())) {
        return std::numeric_limits<T>::max();
    }
    return static_cast<T>(v);
}

template <typename T>
[[nodiscard]] constexpr T saturating_cast(double v) noexcept {
    static_assert(std::is_integral_v<T>, "saturating_cast is for integer targets");

    if (!(v > static_cast<double>(std::numeric_limits<T>::lowest()))) {
        return std::numeric_limits<T>::lowest();
    }
    if (v >= static_cast<double>(std::numeric_limits<T>::max())) {
        return std::numeric_limits<T>::max();
    }
    return static_cast<T>(v);
}

[[nodiscard]] inline bool is_usable(float v) noexcept {
    return std::isfinite(v) && v != 0.0f;
}

}
