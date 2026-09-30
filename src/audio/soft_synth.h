#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace andromeda::audio {

class SoftSynth {
public:
    virtual ~SoftSynth() = default;

    virtual void note_on(std::uint8_t channel, std::uint8_t key, std::uint8_t velocity) = 0;
    virtual void note_off(std::uint8_t channel, std::uint8_t key) = 0;
    virtual void control(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) = 0;
    virtual void pitch_bend(std::uint8_t channel, int bend) = 0;

    virtual void all_notes_killed() = 0;
    virtual void reset_control() = 0;

    virtual void read_samples(float* out, std::size_t frames) = 0;

    [[nodiscard]] virtual bool load_soundfonts(const std::vector<std::string>& paths) = 0;
};

std::unique_ptr<SoftSynth> create_soft_synth(unsigned int sample_rate);

std::vector<std::string> default_soundfont_paths();

}
