#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "audio/midi_audio_engine.h"

class RtMidiIn;
class RtMidiOut;

namespace andromeda::audio {

class MIDIDevices : public MIDIAudioEngine {
public:
    static std::expected<std::unique_ptr<MIDIDevices>, std::string> create();

    ~MIDIDevices() override;

    MIDIDevices(const MIDIDevices&) = delete;
    MIDIDevices& operator=(const MIDIDevices&) = delete;

    std::expected<void, std::string> connect_out_port(std::size_t idx);
    std::expected<void, std::string> connect_in_port(std::size_t idx);

    std::expected<void, std::string> send_raw_event(std::span<const std::uint8_t> raw_event);

    [[nodiscard]] const std::vector<std::string>& get_midi_in_port_names() const {
        return midi_in_port_names_;
    }
    [[nodiscard]] const std::vector<std::string>& get_midi_out_port_names() const {
        return midi_out_port_names_;
    }
    [[nodiscard]] std::optional<std::size_t> get_curr_in_port() const { return curr_midi_in_port_; }
    [[nodiscard]] std::optional<std::size_t> get_curr_out_port() const {
        return curr_midi_out_port_;
    }

    void init_audio() override;
    void close_stream() override;
    std::expected<void, std::string> send_event(std::span<const std::uint8_t> raw_event) override;

private:
    MIDIDevices();

    std::vector<std::string> midi_in_port_names_;
    std::vector<std::string> midi_out_port_names_;

    std::optional<std::size_t> curr_midi_in_port_;
    std::optional<std::size_t> curr_midi_out_port_;
    std::size_t preferred_out_port_ = 0;

    std::unique_ptr<RtMidiIn> in_connection_;
    std::unique_ptr<RtMidiOut> out_connection_;
};

}
