#include "audio/midi_devices.h"

#include <rtmidi/RtMidi.h>

#include <format>

#include "util/debugger.h"

namespace andromeda::audio {

using util::Debugger;

namespace {

void on_midi_in(double stamp, std::vector<unsigned char>* message, void*) {
    if (message == nullptr) {
        return;
    }

    std::string bytes = "[";
    for (std::size_t i = 0; i < message->size(); ++i) {
        bytes += std::format("{}{}", i == 0 ? "" : ", ", (*message)[i]);
    }
    bytes += "]";

    Debugger::log(std::format("At {}ms: {}", stamp * 1000.0, bytes));
}

}

MIDIDevices::MIDIDevices() = default;
MIDIDevices::~MIDIDevices() = default;

std::expected<std::unique_ptr<MIDIDevices>, std::string> MIDIDevices::create() {
    auto s = std::unique_ptr<MIDIDevices>(new MIDIDevices());

    try {
        RtMidiIn midi_in;
        RtMidiOut midi_out;

        const unsigned int in_count = midi_in.getPortCount();
        const unsigned int out_count = midi_out.getPortCount();

        for (unsigned int i = 0; i < in_count; ++i) {
            s->midi_in_port_names_.push_back(midi_in.getPortName(i));
            Debugger::log(std::format("IN({}): {}", i, s->midi_in_port_names_[i]));
        }

        for (unsigned int i = 0; i < out_count; ++i) {
            s->midi_out_port_names_.push_back(midi_out.getPortName(i));
            Debugger::log(std::format("OUT({}): {}", i, s->midi_out_port_names_[i]));
        }
    } catch (const RtMidiError& e) {
        return std::unexpected(e.getMessage());
    }

    if (auto r = s->connect_in_port(0); !r) {
        return std::unexpected(r.error());
    }
    // fixed rust bug: opened output 0 here while kdmapi plays; init_audio opens it when chosen

    return s;
}

std::expected<void, std::string> MIDIDevices::connect_out_port(std::size_t idx) {
    out_connection_.reset();
    curr_midi_out_port_.reset();

    if (midi_out_port_names_.empty()) {
        Debugger::log_warning("No MIDI outputs to connect to.");
        return {};
    }

    if (idx >= midi_out_port_names_.size()) {
        return std::unexpected("Invalid output port index");
    }

    try {
        auto conn_out = std::make_unique<RtMidiOut>();
        conn_out->openPort(static_cast<unsigned int>(idx), "Andromeda out");
        Debugger::log(std::format("Connected to OUT({}): {}", idx, midi_out_port_names_[idx]));

        out_connection_ = std::move(conn_out);
        curr_midi_out_port_ = idx;
        preferred_out_port_ = idx;
    } catch (const RtMidiError& e) {
        return std::unexpected(e.getMessage());
    }

    return {};
}

std::expected<void, std::string> MIDIDevices::connect_in_port(std::size_t idx) {
    if (midi_in_port_names_.empty()) {
        Debugger::log_warning("No MIDI inputs to connect to.");
        return {};
    }

    if (idx >= midi_in_port_names_.size()) {
        return std::unexpected("Invalid input port index");
    }

    try {
        auto conn_in = std::make_unique<RtMidiIn>();
        conn_in->ignoreTypes(false, false, false);
        conn_in->setCallback(&on_midi_in);
        conn_in->openPort(static_cast<unsigned int>(idx), "Andromeda in");
        Debugger::log(std::format("Connected to IN({}): {}", idx, midi_in_port_names_[idx]));

        in_connection_ = std::move(conn_in);
        curr_midi_in_port_ = idx;
    } catch (const RtMidiError& e) {
        return std::unexpected(e.getMessage());
    }

    return {};
}

std::expected<void, std::string> MIDIDevices::send_raw_event(
    std::span<const std::uint8_t> raw_event) {
    if (out_connection_) {
        try {
            out_connection_->sendMessage(raw_event.data(), raw_event.size());
        } catch (const RtMidiError& e) {
            return std::unexpected(e.getMessage());
        }
    }

    return {};
}

void MIDIDevices::init_audio() {
    if (!curr_midi_out_port_.has_value()) {
        // fixed rust bug: this opened the first midi input, replacing the chosen input port
        const std::size_t port =
            preferred_out_port_ < midi_out_port_names_.size() ? preferred_out_port_ : 0;
        if (auto r = connect_out_port(port); !r) {
            Debugger::log_error(std::format(
                "Something went wrong when trying to connect to MIDI out. Details: {}", r.error()));
        }
        return;
    }

    (void)connect_out_port(*curr_midi_out_port_);
}

void MIDIDevices::close_stream() {
    out_connection_.reset();
    curr_midi_out_port_.reset();
}

std::expected<void, std::string> MIDIDevices::send_event(std::span<const std::uint8_t> raw_event) {
    return send_raw_event(raw_event);
}

}
