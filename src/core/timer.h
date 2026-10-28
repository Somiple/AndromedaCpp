#pragma once

#include <chrono>

namespace andromeda::util {

class Timer {
public:
    void start() { instant_ = std::chrono::steady_clock::now(); }

    [[nodiscard]] float elapsed() const {
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - instant_).count();
    }

    float get_delta_time() {
        const float now = elapsed();
        const float delta = now - last_time_;
        last_time_ = now;
        return delta;
    }

private:
    std::chrono::steady_clock::time_point instant_ = std::chrono::steady_clock::now();
    float last_time_ = 0.0f;
};

}
