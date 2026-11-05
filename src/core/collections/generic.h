#pragma once

#include <vector>
#include <ranges>

namespace andromeda::util::collections::generic {

// input array must be sorted. min incluive, max exclusive.
template <std::ranges::contiguous_range R, typename Value,
    typename T, typename Predicate>
inline std::vector<std::size_t> select_ranged(const R& array, Value&& value, T min, T max,
    Predicate&& predicate = [](const auto&) { return true; }, bool inclusive = false) {
    std::vector<std::size_t> result{};

    for (std::size_t i = 0; i < std::ranges::size(array); i++) {
        const auto& element = array[i];
        const T val = value(element);

        if (val < min) continue;
        if (inclusive ? (val > max) : (val >= max)) break;

        if (!predicate(element)) continue;
        result.push_back(i);
    }

    return result;
}

}