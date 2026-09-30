#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <type_traits>

#include <imgui.h>

namespace andromeda::app {

class NumberField {
public:
    virtual ~NumberField() = default;

    virtual bool show(const char* label, float width) = 0;
    [[nodiscard]] virtual bool has_changed() const = 0;

    [[nodiscard]] virtual float as_f32() const = 0;
    [[nodiscard]] virtual std::uint8_t as_u8() const = 0;
};

template <typename T>
class NumericField final : public NumberField {
public:
    static_assert(std::is_arithmetic_v<T>, "NumericField only holds arithmetic types");

    NumericField() : NumericField(T{}, std::nullopt, std::nullopt) {}

    NumericField(T initial, std::optional<T> min_value, std::optional<T> max_value)
        : min_value_(min_value), max_value_(max_value), value_(initial) {
        write_buffer(initial);
    }

    bool show(const char* label, float width = 0.0f) override {
        changed = false;

        ImGui::PushID(this);
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        if (width > 0.0f) {
            ImGui::SetNextItemWidth(width);
        }
        ImGui::InputText("##value", buffer_.data(), buffer_.size());
        if (ImGui::IsItemDeactivated()) {
            update_buffer();
        }
        ImGui::PopID();

        if (changed && on_change) {
            on_change();
        }
        return changed;
    }

    [[nodiscard]] bool has_changed() const override { return changed; }
    [[nodiscard]] float as_f32() const override { return static_cast<float>(value_); }
    [[nodiscard]] std::uint8_t as_u8() const override {
        return static_cast<std::uint8_t>(std::clamp<double>(static_cast<double>(value_), 0.0, 255.0));
    }

    [[nodiscard]] T value() const { return value_; }

    void set_value(T val) {
        write_buffer(val);
        update_buffer();
    }

    bool changed = false;
    std::function<void()> on_change;

private:
    void write_buffer(T val) {
        if constexpr (std::is_floating_point_v<T>) {
            std::snprintf(buffer_.data(), buffer_.size(), "%g", static_cast<double>(val));
        } else if constexpr (std::is_signed_v<T>) {
            std::snprintf(buffer_.data(), buffer_.size(), "%lld", static_cast<long long>(val));
        } else {
            std::snprintf(buffer_.data(), buffer_.size(), "%llu",
                          static_cast<unsigned long long>(val));
        }
    }

    void update_buffer() {
        T parsed{};
        if (parse(buffer_.data(), parsed)) {
            if (min_value_ && parsed < *min_value_) {
                parsed = *min_value_;
            }
            if (max_value_ && parsed > *max_value_) {
                parsed = *max_value_;
            }

            value_ = parsed;
            write_buffer(parsed);
            changed = true;
        } else {
            write_buffer(value_);
            changed = false;
        }
    }

    static bool parse(const char* text, T& out) {
        while (*text == ' ') {
            ++text;
        }
        if (*text == '\0') {
            return false;
        }

        if constexpr (std::is_floating_point_v<T>) {
            char* end = nullptr;
            const double v = std::strtod(text, &end);
            if (end == text) {
                return false;
            }
            out = static_cast<T>(v);
            return true;
        } else {
            char* end = nullptr;
            errno = 0;
            if constexpr (std::is_signed_v<T>) {
                const long long v = std::strtoll(text, &end, 10);
                if (end == text) {
                    return false;
                }
                out = static_cast<T>(std::clamp<long long>(
                    v, static_cast<long long>(std::numeric_limits<T>::lowest()),
                    static_cast<long long>(std::numeric_limits<T>::max())));
            } else {
                if (*text == '-') {
                    return false;
                }
                const unsigned long long v = std::strtoull(text, &end, 10);
                if (end == text) {
                    return false;
                }
                out = static_cast<T>(std::min<unsigned long long>(
                    v, static_cast<unsigned long long>(std::numeric_limits<T>::max())));
            }
            return true;
        }
    }

    std::array<char, 64> buffer_{};
    std::optional<T> min_value_;
    std::optional<T> max_value_;
    T value_{};
};

}
