#pragma once

#include <span>

namespace andromeda::audio {

class Limiter {
public:
    Limiter(float attack, float release, float sample_rate)
        : attack(attack * sample_rate), falloff(release * sample_rate) {}

    void apply_limiter(std::span<float> buffer);

    float attack;
    float falloff;

private:
    float loudness_l_ = 1.0f;
    float loudness_r_ = 1.0f;
    float velocity_r_ = 0.0f;
    float velocity_l_ = 0.0f;
    float strength_ = 1.0f;
    float min_thresh_ = 0.4f;
};

}
