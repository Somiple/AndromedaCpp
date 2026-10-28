#pragma once

#include <concepts>
#include <type_traits>

namespace andromeda::util {

template <typename T>
concept FlagNum = std::unsigned_integral<T> && !std::same_as<T, bool>;

template <FlagNum T>
class FlagsHelper {
public:
    FlagsHelper() : flags_(T{}) {}
    explicit FlagsHelper(T default_flags) : flags_(default_flags) {}

    [[nodiscard]] bool get_flag(T flag) const { return (flags_ & flag) != T{}; }

    void set_flag(T flag, bool value) {
        if (value) {
            enable_flag(flag);
        } else {
            disable_flag(flag);
        }
    }

    void enable_flag(T flag) { flags_ = static_cast<T>(flags_ | flag); }

    void disable_flag(T flag) { flags_ = static_cast<T>(flags_ & static_cast<T>(~flag)); }

private:
    T flags_;
};

}
