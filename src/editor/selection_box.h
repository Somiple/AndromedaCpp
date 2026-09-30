#pragma once

#include <concepts>
#include <utility>

namespace andromeda::editor {

template <typename T>
concept PrimInt = std::integral<T>;

template <PrimInt T, PrimInt U>
class SelectionBox {
public:
    SelectionBox() = default;

    explicit SelectionBox(std::pair<T, U> pos)
        : start_x_(pos.first), end_x_(pos.first), start_y_(pos.second), end_y_(pos.second) {}

    SelectionBox(std::pair<T, U> a, std::pair<T, U> b) {
        if (a.first > b.first) {
            start_x_ = b.first;
            end_x_ = a.first;
        } else {
            start_x_ = a.first;
            end_x_ = b.first;
        }

        if (a.second > b.second) {
            start_y_ = b.second;
            end_y_ = a.second;
        } else {
            start_y_ = a.second;
            end_y_ = b.second;
        }
    }

    void init_from(std::pair<T, U> start_pos) {
        start_x_ = start_pos.first;
        end_x_ = start_pos.first;
        start_y_ = start_pos.second;
        end_y_ = start_pos.second;
    }

    [[nodiscard]] std::pair<T, U> top_left() const { return {start_x_, start_y_}; }
    [[nodiscard]] std::pair<T, U> top_right() const { return {end_x_, start_y_}; }
    [[nodiscard]] std::pair<T, U> bottom_left() const { return {start_x_, end_y_}; }
    [[nodiscard]] std::pair<T, U> bottom_right() const { return {end_x_, end_y_}; }

private:
    T start_x_{};
    T end_x_{};
    U start_y_{};
    U end_y_{};
};

}
