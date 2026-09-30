#include "audio/prerender_engine/limiter.h"

#include <cmath>

namespace andromeda::audio {

void Limiter::apply_limiter(std::span<float> buffer) {
    const std::size_t count = buffer.size();

    // fixed rust bug: read buffer[i + 1] past the end of an odd-length buffer
    for (std::size_t i = 0; i + 1 < count; i += 2) {
        float l = std::abs(buffer[i]);
        float r = std::abs(buffer[i + 1]);

        if (loudness_l_ > l) {
            loudness_l_ = (loudness_l_ * falloff + l) / (falloff + 1.0f);
        } else {
            loudness_l_ = (loudness_l_ * attack + l) / (attack + 1.0f);
        }

        if (loudness_r_ > r) {
            loudness_r_ = (loudness_r_ * falloff + r) / (falloff + 1.0f);
        } else {
            loudness_r_ = (loudness_r_ * attack + r) / (attack + 1.0f);
        }

        if (loudness_l_ < min_thresh_) {
            loudness_l_ = min_thresh_;
        }
        if (loudness_r_ < min_thresh_) {
            loudness_r_ = min_thresh_;
        }

        l = buffer[i] / (loudness_l_ * strength_ + 2.0f * (1.0f - strength_)) / 2.0f;
        r = buffer[i + 1] / (loudness_r_ * strength_ + 2.0f * (1.0f - strength_)) / 2.0f;

        if (i != 0) {
            const float dl = std::abs(buffer[i] - l);
            const float dr = std::abs(buffer[i + 1] - r);

            if (velocity_l_ > dl) {
                velocity_l_ = (velocity_l_ * falloff + dl) / (falloff + 1.0f);
            } else {
                velocity_l_ = (velocity_l_ * attack + dl) / (attack + 1.0f);
            }

            if (velocity_r_ > dr) {
                velocity_r_ = (velocity_r_ * falloff + dr) / (falloff + 1.0f);
            } else {
                velocity_r_ = (velocity_r_ * attack + dr) / (attack + 1.0f);
            }
        }

        buffer[i] = l;
        buffer[i + 1] = r;
    }
}

}
