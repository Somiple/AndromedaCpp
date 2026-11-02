#include "editor/editing/data_editing.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include "editor/editor_controller.h"
#include "editor/project/project_manager.h"
#include "ui/app/main_window.h"
#include "core/debugger.h"

namespace {
   
using andromeda::midi::MIDITick;

struct RampSpec {
    MIDITick min_tick;
	MIDITick max_tick;
    bool snap;
    double tick_interval;
};

template <typename Events, typename Predicate, typename MakeMutator, typename Convert,
    typename ValueAt, typename Replace, typename Add>
void add_and_replace_in_between(Events& events, const RampSpec& ramp, Predicate predicate,
    MakeMutator make_mutator, Convert convert_value,
    ValueAt value_at, Replace replacer, Add adder) {
    const auto [min_tick, max_tick, snap, tick_interval] = ramp;

    const auto factor_at = [&](MIDITick t) {
        return max_tick > min_tick
            ? static_cast<double>(t - min_tick) / static_cast<double>(max_tick - min_tick)
            : 0.0;
        };

    std::vector<std::size_t> ids{};
    std::vector<MIDITick> ticks{};
    for (std::size_t i = 0; i < events.size(); i++) {
        const auto& event = events[i];
        if (event.tick < min_tick) continue;
        if (event.tick > max_tick) break;
        if (!predicate(event)) continue;
        ids.push_back(i);
        ticks.push_back(event.tick);
    }

    auto ev_mutator = make_mutator(ids);

    std::vector<double> coerced_values;
    coerced_values.reserve(ids.size());
    for (std::size_t k = 0; k < ids.size(); k++) {
        const double coerced = convert_value(value_at(factor_at(ticks[k])));
        replacer(ev_mutator, ids[k], coerced);
        coerced_values.push_back(coerced);
    }

    if (max_tick == min_tick && !ids.empty()) return;
    if (max_tick < min_tick) return;

    std::size_t next = 0;
    MIDITick tick = min_tick;
    double interval_id = 1; // multiply instead of accumulate so rounding doesn't drift
    std::optional<double> last_value;

    while (true) {
        bool already_exists = false;
        while (next < ticks.size() && ticks[next] <= tick) {
            if (ticks[next] == tick) {
                last_value = coerced_values[next];
                already_exists = true;
            }
            ++next;
        }

        const double coerced = convert_value(value_at(factor_at(tick)));
        if (!already_exists && (!last_value || *last_value != coerced)) {
            adder(ev_mutator, tick, coerced);
            last_value = coerced;
        }

        if (tick >= max_tick) break;

        if (snap) {
            const auto stepped = min_tick + static_cast<MIDITick>(std::llround(tick_interval * interval_id++));
            tick = std::min(stepped, max_tick);
        } else {
            tick++;
        }
    }
}

}

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

	// todo: define a separate "safe input" area for the data editor (and other components), so that clicks outside the rect don't trigger edits
    if (!is_mouse_over()) {
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
    /*if (get_flag(DATA_EDIT_MOUSE_DOWN_ON_UI | DATA_EDIT_CLICKED_IN_RECT)) {
        return;
    }

    if (get_flag(DATA_EDIT_ANY_DIALOG_OPEN | DATA_EDIT_MOUSE_OVER_UI)) {
        return;
    }*/

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
    if (!_overlay_drawer)
        _overlay_drawer = std::make_unique<overlays::OverlayDrawer>(&context());

    const auto& work_rect = context().rect;
    const auto& mouse_pos_norm = *mouse_pos();

	// map the mouse to the work rect, clamping to the rect's bounds
	Vector2<float> mouse_clamped = mouse_pos_norm;
	mouse_clamped.x = std::clamp((mouse_pos_norm.x - work_rect.left) / work_rect.width, 0.0f, 1.0f);
	mouse_clamped.y = std::clamp((mouse_pos_norm.y - work_rect.top) / work_rect.height, 0.0f, 1.0f);
    mouse_pos_ = mouse_clamped;
    update_stats();
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

    const auto& nav = navigation()->value;

    auto map_point = [&](util::math::Vector2<float> point) {
		// assuming the point's x and y are normalized to the work rect, map them to the data space
        midi::MIDITick x_tick =
            static_cast<midi::MIDITick>(
                std::llround((double)point.x * (double)nav.zoom_ticks_smoothed + (double)nav.tick_pos_smoothed));
        return std::pair{ x_tick, 1.0f - point.y };
    };

    auto [tick_a, value_a] = map_point(mouse_anchor_);
    auto [tick_b, value_b] = map_point(mouse_pos_);
    
    if (tick_a > tick_b) {
        std::swap(tick_a, tick_b);
        std::swap(value_a, value_b);
    }

    set_events_in_between(tick_a, value_a, tick_b, value_b);
}

void DataEditing::select_mouse_down() {
    const KeyModifierState& key_modifiers = *key_modifier_state();
    m_active_gesture = selection_gesture_of(key_modifiers.shift, key_modifiers.alt);

    overlays::SelectionType selection_type = overlays::SelectionType::AddSelect;

    switch (m_active_gesture) {
    case SelectionGesture::Subtract:
        selection_type = overlays::SelectionType::RemoveSelect;
        break;
    case SelectionGesture::Add:
    case SelectionGesture::Replace:
        selection_type = overlays::SelectionType::AddSelect;
        break;
    default:
        break;
    }

	const auto& nav = navigation()->value;

	const midi::MIDITick mouse_tick = static_cast<midi::MIDITick>(std::llround(mouse_pos_.x * nav.zoom_ticks_smoothed + nav.tick_pos_smoothed));
	const midi::MIDITick mouse_tick_snapped = _controller->get_project_manager()->get_timeline()->snap_nearest(mouse_tick);

    begin_live_selection_box(mouse_tick_snapped, selection_type);
}

void DataEditing::select_mouse_move() {
    const auto& nav = navigation()->value;
    const midi::MIDITick mouse_tick = static_cast<midi::MIDITick>(std::llround(mouse_pos_.x * nav.zoom_ticks_smoothed + nav.tick_pos_smoothed));
    const midi::MIDITick mouse_tick_snapped = _controller->get_project_manager()->get_timeline()->snap_nearest(mouse_tick);

    if (live_selection_box) {
        update_live_selection_box(mouse_tick_snapped);
    }
}

void DataEditing::select_mouse_up() {
    const SelectionGesture gesture = m_active_gesture;
    m_active_gesture = SelectionGesture::Replace;

    if (live_selection_box) {
        const auto [min, max] = live_selection_box->bounds();
        end_live_selection_box();

        // TODO: actually select the events in range
	}
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
        }
        else if (matching_ids.empty()) {
            channel_mut.add_event(ChannelEvent{
                .tick = tick,
                .channel = static_cast<std::uint8_t>(curr_source_.channel & 0xF),
                .event_type = midi::ProgramChange{.program = program_id }
                });
        }

        tx.commit();

        bump_revision();
        return true;
    }
    default: {
        const double value_normalized = (value_clamped - min) / (max - min);
        set_events_in_between(tick, value_normalized, tick, value_normalized);
        return true;
    }
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

    bump_revision();
}

void DataEditing::set_events_in_between(MIDITick min_tick, double min_val, MIDITick max_tick, double max_val,
    bool force_no_snap) {
    const std::uint16_t curr_track = _controller->get_active_track();

    const auto& src = curr_source_.event_source;
    const auto src_info = EventSources::get_source(src);
    const auto [min, max] = src_info->info.range;
    const auto min_mapped = min_val * (max - min) + min;
    const auto max_mapped = max_val * (max - min) + min;

	// snap min and max ticks to the nearest grid if snapping is enabled and not forced off
    if (!force_no_snap && tool_.resolution > 0) {
		snap_tick_to_resolution(min_tick);
        snap_tick_to_resolution(max_tick);
    }

    const auto weighted_mix = [&](double x, double mn, double mx) {
        const bool is_linear = tool_.tool_kind == DataEditToolKind::Line ||
            tool_.curve_kind == DataEditCurveKind::Linear;
        const double weight = is_linear ? x : curve_weight(tool_.curve_kind, x, tool_.influence);
        return (1.0f - weight) * mn + weight * mx;
    };

    // deciding whether or not note velocitie sho
    const RampSpec ramp{ min_tick, max_tick, !force_no_snap && tool_.resolution > 0, get_tick_interval() };
    const auto value_at = [&](double x) { return weighted_mix(x, min_mapped, max_mapped); };

    switch (src) {
    case EventSourceKind::Velocity: {
        const std::uint8_t min_vel = std::clamp<std::uint8_t>((std::uint8_t)std::lround(min_mapped), std::lround(min), std::lround(max));
        const std::uint8_t max_vel = std::clamp<std::uint8_t>((std::uint8_t)std::lround(max_mapped), std::lround(min), std::lround(max));
        set_note_velocities_ranged(min_tick, min_vel, max_tick, max_vel);
        break;
    }
    case EventSourceKind::Tempo: {
        auto& metas = *_controller->get_project_manager()->get_metas();
		auto tx = _controller->begin_action("Set tempo");

        auto bpm_to_us = [](double bpm) -> std::uint32_t {
            const double raw_us = 60000000.0 / bpm;
            const std::uint32_t bpm_us = static_cast<std::uint32_t>(std::llround(std::clamp(raw_us, 1.0, 16777215.0)));
            return bpm_us;
        };

        auto us_to_bpm = [](std::uint32_t us) -> double {
            return 60000000.0 / static_cast<double>(us);
			};

        add_and_replace_in_between(metas, ramp,
            [&](const midi::MetaEvent& evt) {
                return evt.event_type == midi::MetaEventType::Tempo;
            },
            [&](std::vector<std::size_t> ids) { return tx.global_meta_events(ids); },
            [&](double value) {
                const double bpm = std::clamp(value, min, max);
                // round-trip through microseconds so coerced values are always a representable tempo
                return us_to_bpm(bpm_to_us(bpm));
            },
            value_at,
            [&](history::MetaEventMutator& mutator, const std::size_t id, const double value) {
                const double bpm = std::clamp(value, min, max);
                mutator.set_tempo(id, bpm);
            },
            [&](history::MetaEventMutator& mutator, const midi::MIDITick tick, const double value) {
                const double bpm = std::clamp(value, min, max);
				const auto bpm_us = bpm_to_us(bpm);
                mutator.add_event(midi::MetaEvent{ .tick = tick,
                                                   .event_type = midi::MetaEventType::Tempo,
                                                   .data = { static_cast<std::uint8_t>((bpm_us >> 16) & 0xFF),
                                                             static_cast<std::uint8_t>((bpm_us >> 8) & 0xFF),
                                                             static_cast<std::uint8_t>(bpm_us & 0xFF) } });
            }
        );

        tx.commit();

        bump_revision();
        break;
    }
    case EventSourceKind::PitchBend: {
        const auto to_raw = [&](double value) -> std::uint16_t {
            const long s = std::clamp(std::lround(value), std::lround(min), std::lround(max));
            return static_cast<std::uint16_t>(s + 8192);
        };

		auto& tracks = *_controller->get_project_manager()->get_tracks();
		auto& events = tracks[_controller->get_active_track()].get_channel_evs();

		auto tx = _controller->begin_action("Set pitch bend");
        add_and_replace_in_between(events, ramp,
            [&](const midi::ChannelEvent& evt) {
                if (curr_source_.channel != 0xFF && evt.channel != curr_source_.channel) return false;
                return std::holds_alternative<midi::PitchBend>(evt.event_type);
            },
            [&](std::vector<std::size_t> ids) { return tx.channel_events(curr_track, ids); },
            [&](double value) {
                return (double)std::llround(value * 16384) / 16384.0;
			},
            value_at,
            [&](history::ChannelEventMutator& mutator, const std::size_t id, const double value) {
                mutator.set_pitch_bend(id, to_raw(value));
            },
            [&](history::ChannelEventMutator& mutator, const midi::MIDITick tick, const double value) {
                const std::uint16_t pb = to_raw(value);
                if (curr_source_.channel == 0xFF) {
                    for (std::uint8_t channel = 0; channel < 16; channel++) {
                        mutator.add_event(midi::ChannelEvent{ .tick = tick, .channel = channel,
                                                      .event_type = midi::PitchBend{ .lsb = static_cast<std::uint8_t>(pb & 0x7F),
                                                                                     .msb = static_cast<std::uint8_t>((pb >> 7) & 0x7F) } });
                    }
				} else {
                    mutator.add_event(midi::ChannelEvent{ .tick = tick,
                                                          .channel = static_cast<std::uint8_t>(curr_source_.channel & 0xF),
                                                          .event_type = midi::PitchBend{.lsb = static_cast<std::uint8_t>(pb & 0x7F),
                                                                                         .msb = static_cast<std::uint8_t>((pb >> 7) & 0x7F) } });
                }
                return static_cast<double>(pb);
            }
		);
        tx.commit();
        bump_revision();
        break;
    }
    case EventSourceKind::ProgramChange: {
		util::Debugger::log_error("Program (patch/instrument) change does not allow ranged edits");
        break;
    }
    case EventSourceKind::ControlChange: {
        auto& tracks = *_controller->get_project_manager()->get_tracks();
        auto& events = tracks[_controller->get_active_track()].get_channel_evs();
		auto tx = _controller->begin_action("Set control change value");

        const auto to_raw = [&](double value) -> std::uint8_t {
            return static_cast<std::uint8_t>(std::clamp(std::lround(value), std::lround(min), std::lround(max)));
		};

        add_and_replace_in_between(events, ramp,
            [&](const midi::ChannelEvent& evt) {
                if (curr_source_.channel != 0xFF && evt.channel != curr_source_.channel) return false;
                if (!std::holds_alternative<midi::Controller>(evt.event_type)) return false;
                const auto& cc = std::get<midi::Controller>(evt.event_type);
                return cc.controller == curr_source_.controller;
            },
            [&](std::vector<std::size_t> ids) { return tx.channel_events(curr_track, ids); },
            [&](double value) {
				return static_cast<double>(to_raw(value));
            },
            value_at,
            [&](history::ChannelEventMutator& mutator, const std::size_t id, const double value) {
                const auto cc = to_raw(value);
                mutator.set_control_change(id, curr_source_.controller, cc);
            },
            [&](history::ChannelEventMutator& mutator, const midi::MIDITick tick, const double value) {
				const auto cc = to_raw(value);
                if (curr_source_.channel == 0xFF) {
                    for (std::uint8_t channel = 0; channel < 16; channel++) {
                        mutator.add_event(midi::ChannelEvent{ .tick = tick, .channel = channel,
                                                      .event_type = midi::Controller{.controller = curr_source_.controller,
                                                                                     .value = cc } });
                    }
                } else {
                    mutator.add_event(midi::ChannelEvent{ .tick = tick,
                                                      .channel = static_cast<std::uint8_t>(curr_source_.channel & 0xF),
                                                      .event_type = midi::Controller{.controller = curr_source_.controller,
                                                                                     .value = cc } });
                }
                return static_cast<double>(cc);
            });
        tx.commit();
        bump_revision();
        break;
    }
    default:
        break;
    }
}

void DataEditing::snap_tick_to_resolution(midi::MIDITick& tick) const {
    const double tick_interval = get_tick_interval();
    // an interval of 1 or less is basically no snapping, so we can skip the calculation
	if (tick_interval <= 1.0) return;

	const std::uint16_t ppq = _controller->get_project_manager()->get_ppq();
	tick = static_cast<midi::MIDITick>(std::llround(static_cast<double>(tick) / tick_interval) * tick_interval);
}

double DataEditing::get_tick_interval() const {
    if (tool_.resolution <= 0) return 1;
    const std::uint16_t ppq = _controller->get_project_manager()->get_ppq();
    return static_cast<double>(ppq) / static_cast<double>(tool_.resolution);
}

void DataEditing::update_last_mouse_data_pos() {
    // mouse_info_.last_data_click_pos = mouse_info_.mouse_data_pos;
    // mouse_info_.last_screen_click_pos = mouse_info_.mouse_screen_pos;
}

void DataEditing::update_stats() {
    const auto& rect = context().rect;

    const auto& src = curr_source_.event_source;
    const auto src_info = EventSources::get_source(src);
    const auto [min, max] = src_info->info.range;

    auto& stats = stats_;

	// we have a small margin because mouse pos was clamped already.
    // values would've never exceeded the range,but we want to avoid showing stats when the mouse is outside the rect
	stats.stats_present = mouse_pos_.x >= 0.01f && mouse_pos_.x <= 0.99f && mouse_pos_.y >= 0.01f && mouse_pos_.y <= 0.99f;

    if (!stats.stats_present) return;

    stats.value_at_mouse = (1.0f - mouse_pos_.y) * (max - min) + min;

    const auto curr_track = _controller->get_active_track();
    const auto& tracks = *_controller->get_project_manager()->get_tracks();

    // calculate current value at mouse
    const auto& nav = navigation()->value;
    midi::MIDITick curr_tick =
        static_cast<midi::MIDITick>(std::llround(mouse_pos_.x * nav.zoom_ticks_smoothed + nav.tick_pos_smoothed));

    switch (src) {
    case EventSourceKind::Velocity: {
        stats.value_at_mouse = std::round(stats.value_at_mouse);
        stats.present_value_at_mouse = stats.value_at_mouse;
        break;
    }
    case EventSourceKind::Tempo: {
        const auto& timeline = *_controller->get_project_manager()->get_timeline();
        stats.present_value_at_mouse = timeline.bpm_at(curr_tick);
        break;
    }
    default: {
        stats.present_value_at_mouse = stats.value_at_mouse;
        break;
    }
    }
}

std::pair<Vector2<float>, Vector2<float>>
DataEditing::get_data_view_line_points() const {
    auto a = mouse_anchor_;
    auto b = mouse_pos_;

    if (a.x > b.x)
        std::swap(a, b);

	// convert from normalized to screen space
	const auto& work_rect = context().rect;
	a.x = a.x * work_rect.width + work_rect.left;
	a.y = a.y * work_rect.height + work_rect.top;
	b.x = b.x * work_rect.width + work_rect.left;
	b.y = b.y * work_rect.height + work_rect.top;
    
    return { a, b };
}

#pragma region selection overlays

overlays::TransformFn DataEditing::make_overlay_transform() const {
    return [this](const overlays::OverlayRect& rect) -> overlays::ScreenRect {
        // rect's x pos is already in midi ticks, and y pos is already normalized
        const auto& rc = context().rect;
        const auto& nav = navigation()->value;

        const double xmin_norm = ((double)rect.first.x - (double)nav.tick_pos_smoothed) / (double)nav.zoom_ticks_smoothed;
		const double xmax_norm = ((double)rect.second.x - (double)nav.tick_pos_smoothed) / (double)nav.zoom_ticks_smoothed; 

        return { Vector2<float>(xmin_norm * rc.width + rc.left, rect.first.y * rc.height + rc.top),
                 Vector2<float>(xmax_norm * rc.width + rc.left, rect.second.y * rc.height + rc.top) };
    };
}

void DataEditing::begin_live_selection_box(midi::MIDITick start, overlays::SelectionType type) {
    if (live_selection_box != nullptr) {
        util::Debugger::log_warning("Live selection box already exists");
        return;
    }
	const editor::timeline::Timeline* timeline = _controller->get_project_manager()->get_timeline();
    // convert mouse pos to midi pos
    live_selection_box = overlay_drawer()->emplace_overlay<overlays::SelectionBox>(OVERLAY_LAYER_LIVE);
	auto& info = live_selection_box->info;
    info.type = type;
	info.start = Vector2<int64_t>(static_cast<int64_t>(start), 0);
	info.end = Vector2<int64_t>(static_cast<int64_t>(start), 1);
}

void DataEditing::update_live_selection_box(midi::MIDITick new_pos) {
    if (live_selection_box == nullptr) {
        util::Debugger::log_warning("No live selection box to update");
        return;
    }

    auto& info = live_selection_box->info;
    info.end.x = static_cast<int64_t>(new_pos);
}

void DataEditing::end_live_selection_box() {
	if (_overlay_drawer) overlay_drawer()->clear_layer(OVERLAY_LAYER_LIVE);
    live_selection_box = nullptr;
}

void DataEditing::add_committed_selection_box(std::pair<midi::MIDITick, midi::MIDITick> range) {
    if (_overlay_drawer == nullptr) {
        util::Debugger::log_warning("No overlay drawer to add committed selection box");
        return;
    }
    auto box = overlay_drawer()->emplace_overlay<overlays::SelectionBox>(OVERLAY_LAYER_COMMITTED);
    auto& info = box->info;
    info.type = overlays::SelectionType::Default; // committed boxes are always replace
    info.start = Vector2<int64_t>(static_cast<int64_t>(range.first), 0);
    info.end = Vector2<int64_t>(static_cast<int64_t>(range.second), 1);
    committed_selection_boxes.push_back(box);
}

void DataEditing::clear_committed_selection_boxes() {
    if (_overlay_drawer) overlay_drawer()->clear_layer(OVERLAY_LAYER_COMMITTED);
    committed_selection_boxes.clear();
}

#pragma endregion

}
