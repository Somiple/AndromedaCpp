#include "editor/editing/data_editing.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include "editor/editor_controller.h"
#include "editor/project/project_manager.h"
#include "ui/app/main_window.h"
#include "core/debugger.h"

namespace andromeda::editor {

using namespace data_edit_flags;
using namespace util::math;
using app::EditorTool;
using app::VS_PianoRoll_DataViewState;

std::uint16_t DataEditing::lane_track() const {
    if (_controller == nullptr) {
        return 0;
    }
    // the controller is the single source of truth for the active track
    return static_cast<std::uint16_t>(_controller->get_active_track());
}

const std::vector<midi::Note>* DataEditing::track_notes(std::uint16_t track) const {
    if (_controller == nullptr) {
        return nullptr;
    }

    ProjectManager* manager = _controller->get_project_manager();
    if (manager == nullptr) {
        return nullptr;
    }

    std::vector<midi::MIDITrack>* tracks = manager->get_tracks();
    if (tracks == nullptr || track >= tracks->size()) {
        return nullptr;
    }

    return &(*tracks)[track].get_notes();
}

const std::vector<midi::ChannelEvent>* DataEditing::track_events(std::uint16_t track) const {
    if (_controller == nullptr) {
        return nullptr;
    }

    ProjectManager* manager = _controller->get_project_manager();
    if (manager == nullptr) {
        return nullptr;
    }

    std::vector<midi::MIDITrack>* tracks = manager->get_tracks();
    if (tracks == nullptr || track >= tracks->size()) {
        return nullptr;
    }

    return &(*tracks)[track].get_channel_evs();
}

const std::vector<midi::MetaEvent>* DataEditing::global_metas() const {
    if (_controller == nullptr) return nullptr;

    ProjectManager* manager = _controller->get_project_manager();
    if (manager == nullptr) return nullptr;

    return manager->get_metas();
}

void DataEditing::on_mouse_down() {
    if (get_flag(DATA_EDIT_MOUSE_OVER_UI)) {
        enable_flag(DATA_EDIT_MOUSE_DOWN_ON_UI);
        return;
    }

    if (get_flag(DATA_EDIT_ANY_DIALOG_OPEN)) {
        return;
    }

    const auto& tool_kind = tool_.tool_kind;
    switch (tool_kind) {
    case DataEditToolKind::Pencil:
        // TODO: live-edit the closest clicked data point (velocity, pitch-bend, cc event, poly)
        break;
    case DataEditToolKind::Line:
    case DataEditToolKind::Curve:
        pencil_mouse_down();
        break;
    case DataEditToolKind::Selector:
        select_mouse_down();
        break;
    case DataEditToolKind::Eraser:
        eraser_mouse_down();
        break;
    }

    enable_flag(DATA_EDIT_CLICKED_IN_RECT);
}

void DataEditing::on_mouse_move() {
    if (get_flag(DATA_EDIT_MOUSE_DOWN_ON_UI | DATA_EDIT_CLICKED_IN_RECT)) {
        return;
    }

    if (get_flag(DATA_EDIT_ANY_DIALOG_OPEN | DATA_EDIT_MOUSE_OVER_UI)) {
        return;
    }

    const auto& tool_kind = tool_.tool_kind;
    switch (tool_kind) {
    case DataEditToolKind::Pencil:
        // TODO: live-edit the closest clicked data point (velocity, pitch-bend, cc event, poly)
        break;
    case DataEditToolKind::Line:
    case DataEditToolKind::Curve:
        pencil_mouse_move();
        break;
    case DataEditToolKind::Selector:
        select_mouse_move();
        break;
    case DataEditToolKind::Eraser:
        eraser_mouse_move();
        break;
    }
}

void DataEditing::on_mouse_up() {
    if (get_flag(DATA_EDIT_MOUSE_DOWN_ON_UI)) {
        disable_flag(DATA_EDIT_MOUSE_DOWN_ON_UI);
        return;
    }

    if (!get_flag(DATA_EDIT_CLICKED_IN_RECT)) {
        return;
    }
    // fixed rust bug: never cleared, so any later release ran the tool's mouse-up
    disable_flag(DATA_EDIT_CLICKED_IN_RECT);

    if (get_flag(DATA_EDIT_MOUSE_OVER_UI | DATA_EDIT_ANY_DIALOG_OPEN)) {
        return;
    }

    const auto& tool_kind = tool_.tool_kind;
    switch (tool_kind) {
    case DataEditToolKind::Pencil:
        // TODO: live-edit the closest clicked data point (velocity, pitch-bend, cc event, poly)
        break;
    case DataEditToolKind::Line:
    case DataEditToolKind::Curve:
        pencil_mouse_up();
        break;
    case DataEditToolKind::Selector:
        select_mouse_up();
        break;
    case DataEditToolKind::Eraser:
        eraser_mouse_up();
        break;
    }
}

void DataEditing::update() {
    const auto& work_rect = context().rect;
    const auto& mouse_pos_norm = *mouse_pos();

    mouse_pos_ = mouse_pos_norm;
}

std::pair<MIDITick, DataNumType> DataEditing::screen_pos_to_data_pos(Vector2<float> screen_pos) const {
    const auto& work_rect = context().rect;
    auto& nav = navigation()->value;
    const float screen_x_norm = (screen_pos.x - work_rect.left) / work_rect.width;

    // fixed rust bug: used a hardcoded 21px row and 200px height instead of the strip rect
    const float screen_y_norm = 1.0f - (screen_pos.y - work_rect.top) / work_rect.height;

    MIDITick screen_x_tick = 0;
    screen_x_tick = static_cast<MIDITick>(screen_x_norm * nav.zoom_ticks_smoothed +
                                            nav.tick_pos_smoothed);

    const DataNumType screen_y_data = scaled_y_from_curr_data(screen_y_norm);

    return {screen_x_tick, screen_y_data};
}

Vector2<float> DataEditing::data_pos_to_screen_pos(std::pair<MIDITick, DataNumType> data_pos) const {
    const auto& work_rect = context().rect;
    auto& nav = navigation()->value;

    float data_x_norm = 0.0f;

    // fixed rust bug: divided x by the key zoom instead of the tick zoom
    data_x_norm = (static_cast<float>(data_pos.first) - nav.tick_pos_smoothed) /
                    nav.zoom_ticks_smoothed;

    const float data_y_norm = unscaled_y_from_curr_data(data_pos.second);

    const float data_x_scr = data_x_norm * work_rect.width + work_rect.left;
    const float data_y_scr = (1.0f - data_y_norm) * work_rect.height + work_rect.top;

    return {data_x_scr, data_y_scr};
}

DataNumType DataEditing::scaled_y_from_curr_data(float y) const {
    switch (data_view_state) {
    case VS_PianoRoll_DataViewState::NoteVelocities:
        return std::clamp(static_cast<DataNumType>(y * 127.0f), static_cast<DataNumType>(0),
                          static_cast<DataNumType>(127));
    case VS_PianoRoll_DataViewState::PitchBend:
        return std::clamp(static_cast<DataNumType>((y * 2.0f - 1.0f) * 8192.0f),
                          static_cast<DataNumType>(-8192), static_cast<DataNumType>(8191));
    default:
        return 0;
    }
}

float DataEditing::unscaled_y_from_curr_data(DataNumType y) const {
    switch (data_view_state) {
    case VS_PianoRoll_DataViewState::NoteVelocities:
        return static_cast<float>(y) / 127.0f;
    case VS_PianoRoll_DataViewState::PitchBend:
        return static_cast<float>(y) / 8192.0f * 0.5f + 0.5f;
    default:
        return 0.0f;
    }
}

void DataEditing::pencil_mouse_down() {
    mouse_anchor_ = mouse_pos_;
    enable_flag(DATA_EDIT_DRAW_EDIT_LINE);
}

void DataEditing::pencil_mouse_move() {}

void DataEditing::pencil_mouse_up() {
    disable_flag(DATA_EDIT_DRAW_EDIT_LINE);

    const auto& work_rect = context().rect;
    const auto& nav = navigation()->value;

    if (work_rect.width <= 0.0f || work_rect.height <= 0.0f) return;

    auto map_point = [&](util::math::Vector2<float> point) {
        auto p = point;
        p.x = std::clamp((p.x - work_rect.left) / work_rect.width, 0.0f, 1.0f);
        p.y = std::clamp((p.y - work_rect.top) / work_rect.height, 0.0f, 1.0f);

        midi::MIDITick x_tick = static_cast<midi::MIDITick>(std::llround((double)p.x * (double)nav.zoom_ticks_smoothed + (double)nav.tick_pos_smoothed));
        return std::pair{ x_tick, 1.0f - p.y };
    };

    auto [tick_a, value_a] = map_point(mouse_anchor_);
    auto [tick_b, value_b] = map_point(mouse_pos_);
    
    if (tick_a > tick_b) {
        std::swap(tick_a, tick_b);
        std::swap(value_a, value_b);
    }

    set_events_in_between(tick_a, value_a, tick_b, value_b);
}

double DataEditing::curve_weight(DataEditCurveKind kind, double t, double curve_exponent) noexcept {
    const double clamped = std::clamp(t, 0.0, 1.0);

    switch (kind) {
    case DataEditCurveKind::Linear:
        return clamped;
    case DataEditCurveKind::Exponential:
        return std::pow(clamped, curve_exponent);
    case DataEditCurveKind::Logarithmic:
        return 1.0 - std::pow(1.0 - clamped, curve_exponent);
    case DataEditCurveKind::SCurve: {
        const double a = std::pow(clamped, curve_exponent);
        const double b = std::pow(1.0 - clamped, curve_exponent);
        return a / (a + b);
    }
    case DataEditCurveKind::Pulse:
        return (clamped < curve_exponent / (1.0 + curve_exponent) ? 0.0 : 1.0);
    }
    return clamped;
}

bool DataEditing::set_value_at_current(double value) {
    const MIDITick tick = _controller->playhead_tick();

    const std::uint16_t curr_track = _controller->get_active_track();
    const auto& src = curr_source_.event_source;
    const auto src_info = EventSources::get_source(src);
    const auto [min, max] = src_info->info.range;
    const double value_clamped = std::clamp(value, min, max);

    switch (src) {
    case EventSourceKind::Velocity: {
        const double vel_normalized = (value_clamped - min) / (max - min);
        set_events_in_between(tick, vel_normalized, tick, vel_normalized);
        return true;
    }
    case EventSourceKind::Tempo: {
        const double tempo_normalized = (value_clamped - min) / (max - min);
        set_events_in_between(tick, tempo_normalized, tick, tempo_normalized);
        return true;
    }
    case EventSourceKind::ProgramChange: {
        const std::uint8_t program_id = static_cast<std::uint8_t>(std::llround(value_clamped));

        std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
        auto& events = tracks->at(curr_track).get_channel_evs();
        
        std::vector<std::size_t> matching_ids{};
        std::array<bool, 16> channels_present{};

        for (std::size_t i = 0; i < events.size(); i++) {
            const auto& event = events[i];

            if (event.tick < tick) continue;
            if (event.tick > tick) break;

            if (curr_source_.channel != 0xFF && curr_source_.channel != event.channel) continue;
            if (!std::holds_alternative<midi::ProgramChange>(event.event_type)) continue;
            matching_ids.push_back(i);
            if (event.channel < 16) channels_present[event.channel] = true;
        }

        auto tx = _controller->begin_action("Change instrument");
        auto channel_mut = tx.channel_events(curr_track, matching_ids);

        if (!matching_ids.empty())
            channel_mut.set_program(matching_ids, program_id);

        if (curr_source_.channel == 0xFF) {
            for (std::uint8_t channel = 0; channel < 16; channel++) {
                if (channels_present[channel]) continue;

                channel_mut.add_event(ChannelEvent{
                    .tick = tick,
                    .channel = channel,
                    .event_type = midi::ProgramChange{.program = program_id }
                    });
            }
        } else if (matching_ids.empty()) {
            channel_mut.add_event(ChannelEvent{
                .tick = tick,
                .channel = static_cast<std::uint8_t>(curr_source_.channel & 0xF),
                .event_type = midi::ProgramChange{.program = program_id }
                });
        }

        tx.commit();

        mark_edited();
        return true;
    }
    default:
        // the remaining sources gain their direct-entry path with their own projection
        return false;
    }
}

void DataEditing::set_note_velocities_ranged(MIDITick min_tick, std::uint8_t min_velocity,
                                             MIDITick max_tick, std::uint8_t max_velocity) {
    const std::uint16_t curr_track = _controller->get_active_track();
    
    std::vector<size_t> note_ids{};
    std::vector<uint8_t> new_velocities{};
    
    {
        std::vector<midi::MIDITrack>* tracks = _controller->get_project_manager()->get_tracks();
        const std::vector<midi::Note>& notes = tracks->at(curr_track).get_notes();

        for (size_t i = 0; i < notes.size(); i++) {
            const Note& note = notes[i];
            MIDITick note_tick = note.get_start();
            if (note_tick < min_tick || note_tick > max_tick) continue;
            
            // fixed rust bug: divided by zero on a zero-width drag
            const float vel_factor =
                max_tick > min_tick ? static_cast<float>(note_tick - min_tick) /
                static_cast<float>(max_tick - min_tick)
                : 0.0f;

            bool is_linear = tool_.tool_kind == DataEditToolKind::Line ||
                tool_.curve_kind == DataEditCurveKind::Linear;

            const float weight = is_linear
                ? vel_factor : curve_weight(tool_.curve_kind, vel_factor, tool_.influence);

            const float vel_mixed =
                (1.0f - weight) * static_cast<float>(min_velocity) +
                weight * static_cast<float>(max_velocity);

            // a velocity of 0 is a note-off in the MIDI format, so the encoded range starts at 1
            const auto vel_mix = static_cast<uint8_t>(
                std::clamp(static_cast<int>(std::lround(vel_mixed)), 1, 127));

            note_ids.push_back(i);
            new_velocities.push_back(vel_mix);
        }
    }

    if (note_ids.empty()) {
        return;
    }

    auto tx = _controller->begin_action("Set note velocities");
    auto notes_mutator = tx.notes(curr_track, note_ids);
    notes_mutator.set_velocity(note_ids, new_velocities);
    
    if (!tx.commit()) {
        util::Debugger::log_error("Failed to commit note velocities action");
    }

    mark_edited();
}

void DataEditing::set_events_in_between(MIDITick min_tick, double min_val, MIDITick max_tick, double max_val) {
    const std::uint16_t curr_track = _controller->get_active_track();

    const auto& src = curr_source_.event_source;
    const auto src_info = EventSources::get_source(src);
    const auto [min, max] = src_info->info.range;
    const auto min_mapped = min_val * (max - min) + min;
    const auto max_mapped = max_val * (max - min) + min;

    const auto weighted_mix = [&](double x, double mn, double mx) {
        const bool is_linear = tool_.tool_kind == DataEditToolKind::Line ||
            tool_.curve_kind == DataEditCurveKind::Linear;
        const double weight = is_linear ? x : curve_weight(tool_.curve_kind, x, tool_.influence);
        return (1.0f - weight) * mn + weight * mx;
    };

    switch (src) {
    case EventSourceKind::Velocity: {
        const std::uint8_t min_vel = std::clamp<std::uint8_t>((std::uint8_t)std::lround(min_mapped), std::lround(min), std::lround(max));
        const std::uint8_t max_vel = std::clamp<std::uint8_t>((std::uint8_t)std::lround(max_mapped), std::lround(min), std::lround(max));
        set_note_velocities_ranged(min_tick, min_vel, max_tick, max_vel);
        break;
    }
    case EventSourceKind::Tempo: {
        // 1. find tempos possibly in-between. will use grid snapping later
        const double min_bpm = std::clamp(min_mapped, min, max);
        const double max_bpm = std::clamp(max_mapped, min, max);

        std::vector<std::size_t> ids_in_between{};
        std::vector<editor::MIDITick> ticks_in_between{};

        auto& metas = *_controller->get_project_manager()->get_metas();
        for (std::size_t i = 0; i < metas.size(); i++) {
            auto& meta = metas[i];
            if (meta.tick < min_tick) continue;
            if (meta.tick > max_tick) break;

            // filter
            const bool filter = meta.event_type == midi::MetaEventType::Tempo;
            if (!filter) continue;

            ids_in_between.push_back(i);
            ticks_in_between.push_back(meta.tick);
        }

        auto tx = _controller->begin_action("Set tempos");
        auto meta = tx.global_meta_events(ids_in_between);
        // no need to compare against the other, arrays are synced
        // modify the tempos found in between first
        if (!ids_in_between.empty()) {
            for (const std::size_t id : ids_in_between) {
                auto& tempo_ev = metas[id];
                const double bpm_factor = (max_tick > min_tick) ?
                    static_cast<double>(tempo_ev.tick - min_tick) / static_cast<double>(max_tick - min_tick)
                    : 0.0f;

                const double bpm_mixed = weighted_mix(bpm_factor, min_bpm, max_bpm);
                meta.set_tempo(id, bpm_mixed);
            }
        }

        // fast path: we already edited a tempo in between
        if (max_tick == min_tick && !ids_in_between.empty()) {
            tx.commit();
            mark_edited();
            break;
        }

        // then add tempos (TODO: use the timeline snap later)
        if (max_tick >= min_tick) {
            std::size_t tick_id = 0;
            std::uint32_t last_us = 16777216;

            for (midi::MIDITick tick = min_tick;; tick++) {
                while (tick_id < ticks_in_between.size() &&
                    ticks_in_between[tick_id] == tick) {
                    tick_id++;
                }

                const bool already_exists = tick_id > 0 && ticks_in_between[tick_id - 1] == tick;

                const double bpm_factor = (max_tick > min_tick) ?
                    static_cast<double>(tick - min_tick) / static_cast<double>(max_tick - min_tick)
                    : 0.0f;

                const double bpm_mixed = weighted_mix(bpm_factor, min_bpm, max_bpm);
                const double raw_us = 60000000.0 / bpm_mixed;

                const std::uint32_t bpm_us = static_cast<std::uint32_t>(std::llround(std::clamp(raw_us, 1.0, 16777215.0)));

                if (already_exists) {
                    last_us = bpm_us;
                } else if (last_us != bpm_us) {
                    meta.add_event(midi::MetaEvent{ .tick = tick,
                                                    .event_type = midi::MetaEventType::Tempo,
                                                    .data = { static_cast<std::uint8_t>((bpm_us >> 16) & 0xFF),
                                                                static_cast<std::uint8_t>((bpm_us >> 8) & 0xFF),
                                                                static_cast<std::uint8_t>(bpm_us & 0xFF) } });
                }

                last_us = bpm_us;

                if (tick == max_tick) break;
            }
        }
        
        tx.commit();

        mark_edited();
        break;
    }
    default:
        break;
    }
}

void DataEditing::update_last_mouse_data_pos() {
    // mouse_info_.last_data_click_pos = mouse_info_.mouse_data_pos;
    // mouse_info_.last_screen_click_pos = mouse_info_.mouse_screen_pos;
}

std::pair<Vector2<float>, Vector2<float>>
DataEditing::get_data_view_line_points() const {
    auto a = mouse_anchor_;
    auto b = mouse_pos_;

    if (a.x > b.x)
        std::swap(a, b);

    return { a, b };
}

}
