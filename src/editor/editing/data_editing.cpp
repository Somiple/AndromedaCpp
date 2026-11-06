#include "editor/editing/data_editing.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include "editor/editor_controller.h"
#include "editor/project/project_manager.h"
#include "ui/app/main_window.h"
#include "core/debugger.h"
#include "core/collections/generic.h"

namespace {
   
using andromeda::midi::MIDITick;
using namespace andromeda::util::collections::generic;

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
    if (max_tick < min_tick) return;

    const auto factor_at = [&](MIDITick t) {
        return max_tick > min_tick
            ? static_cast<double>(t - min_tick) / static_cast<double>(max_tick - min_tick)
            : 0.0;
        };

    std::vector<std::size_t> ids = select_ranged(events,
        [](const auto& e) { return e.tick; },
        min_tick, max_tick, predicate, true);
    std::vector<MIDITick> ticks{};

    ticks.reserve(ids.size());
    for (const std::size_t id : ids) { ticks.push_back(events[id].tick); }

    auto ev_mutator = make_mutator(ids);

    std::vector<double> coerced_values;
    coerced_values.reserve(ids.size());
    for (std::size_t k = 0; k < ids.size(); k++) {
        const double coerced = convert_value(value_at(factor_at(ticks[k])));
        replacer(ev_mutator, ids[k], coerced);
        coerced_values.push_back(coerced);
    }

    if (max_tick == min_tick && !ids.empty()) return;

    std::size_t next = 0;
    MIDITick tick = min_tick;
    double interval_id = 1; // multiply instead of accumulate so rounding doesn't drift
    std::optional<double> last_value;

    while (true) {
        bool already_exists = false;
        while (next < ticks.size() && ticks[next] <= tick) {
            last_value = coerced_values[next];
            if (ticks[next] == tick) {
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

template<typename T, typename SelectionGetter, typename F>
void with_selection(const std::vector<T>* events, SelectionGetter&& selection_getter,
    std::uint16_t track, F&& func) {
    if (!events) return;
    const auto& evs = *events;

    const auto& selection = selection_getter();
    const auto& ids = selection->get_selected_ids_in_track(track);
    if (!ids || ids->empty()) return;

    func(evs, *ids);
}

inline const andromeda::midi::MetaEvent DEFAULT_TEMPO = andromeda::midi::MetaEvent{
    .tick = 0,
    .event_type = andromeda::midi::MetaEventType::Tempo,
    .data = { 0x07, 0xA1, 0x20 }
};

}

namespace andromeda::editor {

using namespace data_edit_flags;
using namespace util::math;
using app::EditorTool;
using app::VS_PianoRoll_DataViewState;

#pragma region Data clipboard

void DataEditClipboard::events_to_clipboard(
    std::vector<midi::ChannelEvent> events, DataEditSource source) {
    copied_source_ = source;
    copied_events_ = std::move(events);
}

void DataEditClipboard::events_to_clipboard(
    std::vector<midi::MetaEvent> events, DataEditSource source) {
    copied_source_ = source;
    copied_events_ = std::move(events);
}

std::vector<midi::MetaEvent>* DataEditClipboard::get_copied_metas() {
    if (!std::holds_alternative<std::vector<midi::MetaEvent>>(copied_events_))
        return nullptr;

    return &std::get<std::vector<midi::MetaEvent>>(copied_events_);
}

std::vector<midi::ChannelEvent>* DataEditClipboard::get_copied_events() {
    if (!std::holds_alternative<std::vector<midi::ChannelEvent>>(copied_events_))
        return nullptr;

    return &std::get<std::vector<midi::ChannelEvent>>(copied_events_);
}

DataEditSource* DataEditClipboard::get_copied_source() {
    if (!copied_source_) return nullptr;
    return &*copied_source_;
}

#pragma endregion

#pragma region Data editor

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

void DataEditing::cleanup() {
    if (live_selection_box) end_live_selection_box();
    clear_committed_selection_boxes();
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

void DataEditing::on_key_down(const KeyState& keys) {
    if (keys.del) {
        delete_selected_events();
    }

    if (keys.copy) {
        copy_selected_events();
    }

    if (keys.paste) {
        paste_selected_events();
    }
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

    // TODO: helper function for getting mouse tick (snapped/not)
	const auto& nav = navigation()->value;
	const midi::MIDITick mouse_tick = static_cast<midi::MIDITick>(std::llround(mouse_pos_.x * nav.zoom_ticks_smoothed + nav.tick_pos_smoothed));
	const midi::MIDITick mouse_tick_snapped = _controller->get_project_manager()->get_timeline()->snap_nearest(mouse_tick);

    if (std::ranges::any_of(
        committed_selection_boxes,
        [&](const auto& box) {
            const auto& bounds = box->bounds();
            return mouse_tick >= bounds.first.x &&
                mouse_tick <= bounds.second.x;
        }
    ) && source_is_mutable()) {
        enable_flag(DATA_EDIT_MOUSE_DOWN_IN_SEL);
        mouse_tick_anchor_ = mouse_tick_snapped;
        return;
    }
    
    if (!live_selection_box) {
        begin_live_selection_box(mouse_tick_snapped, selection_type);
    }
}

void DataEditing::select_mouse_move() {
    const auto& nav = navigation()->value;
    const midi::MIDITick mouse_tick = static_cast<midi::MIDITick>(std::llround(mouse_pos_.x * nav.zoom_ticks_smoothed + nav.tick_pos_smoothed));
    const midi::MIDITick mouse_tick_snapped = _controller->get_project_manager()->get_timeline()->snap_nearest(mouse_tick);

    if (live_selection_box) {
        update_live_selection_box(mouse_tick_snapped);
    } else if (get_flag(DATA_EDIT_MOUSE_DOWN_IN_SEL)) {
        const SignedMIDITick tick_change =
            static_cast<SignedMIDITick>(mouse_tick_snapped) - static_cast<SignedMIDITick>(mouse_tick_anchor_);

        // TODO: visualize moving the data points
        offset_committed_selection_boxes(tick_change);
    }
}

void DataEditing::select_mouse_up() {
    const SelectionGesture gesture = m_active_gesture;
    m_active_gesture = SelectionGesture::Replace;

    bool was_moving = get_flag(DATA_EDIT_MOUSE_DOWN_IN_SEL);
    disable_flag(DATA_EDIT_MOUSE_DOWN_IN_SEL);

    const auto& src_origin = EventSources::get_source(curr_source_.event_source)->info.source_origin;

    if (live_selection_box) {
        const auto range = live_selection_box->bounds();
        const auto min = range.first.x;
        const auto max = range.second.x;
        end_live_selection_box();

        const auto& src = curr_source_.event_source;
        const auto& active_track = _controller->get_active_track();

        const auto& select = [&](const auto* array, auto predicate,
            auto tick_getter, auto selection_mutator, auto track) {
            if (!array) return;

            const auto& arr = *array;
            std::vector<std::size_t> to_select =
                select_ranged(arr, tick_getter, min, max, predicate);

            auto tx = _controller->begin_action("Select/deselect");
            auto selection = selection_mutator(tx);

            if (to_select.empty() && gesture == SelectionGesture::Replace) {
                clear_committed_selection_boxes();
                selection.clear_track(track);
                tx.commit();
                return;
            }

            if (gesture == SelectionGesture::Replace) {
                clear_committed_selection_boxes();
                selection.set(track, to_select);
            } else if (gesture == SelectionGesture::Add) {
                selection.add(track, to_select);
            } else if (gesture == SelectionGesture::Subtract) {
                selection.remove(track, to_select);
            }

            if (gesture != SelectionGesture::Subtract)
                add_committed_selection_box({ min, max });

            tx.commit();
        };

        const auto& select_channel_events = [&](auto predicate) {
            select(track_events(active_track),
                [&](const midi::ChannelEvent& event) {
                    if (curr_source_.channel != 0xFF && curr_source_.channel != event.channel)
                        return false;
                    return predicate(event);
                },
                [](const midi::ChannelEvent& event) { return event.tick; },
                [](history::Transaction& tx) { return tx.selection().channel_events(); },
                active_track);
        };

        switch (src) {
            case EventSourceKind::Velocity: {
                select(track_notes(active_track),
                    [](const midi::Note& note) { return true; },
                    [](const midi::Note& note) { return note.get_start(); },
                    [](history::Transaction& tx) { return tx.selection(); },
                    active_track);
                break;
            }
            case EventSourceKind::Tempo: {
                select(global_metas(),
                    [](const midi::MetaEvent& meta) { return meta.event_type == midi::MetaEventType::Tempo; },
                    [](const midi::MetaEvent& meta) { return meta.tick; },
                    [](history::Transaction& tx) { return tx.selection().meta_events(); },
                    history::kGlobalTrack);
                break;
            }
            case EventSourceKind::PitchBend: {
                select_channel_events([&](const midi::ChannelEvent& event) {
                    return std::holds_alternative<midi::PitchBend>(event.event_type);
                });
                break;
            }
            case EventSourceKind::ProgramChange: {
                select_channel_events([&](const midi::ChannelEvent& event) {
                    return std::holds_alternative<midi::ProgramChange>(event.event_type);
                });
                break;
            }
            case EventSourceKind::ControlChange: {
                select_channel_events([&](const midi::ChannelEvent& event) {
                    return std::holds_alternative<midi::Controller>(event.event_type) &&
                        std::get<midi::Controller>(event.event_type).controller == curr_source_.controller;
                });
                break;
            }
            case EventSourceKind::ChannelPressure: {
                select_channel_events([&](const midi::ChannelEvent& event) {
                    return std::holds_alternative<midi::ChannelAftertouch>(event.event_type);
                });
                break;
            }
            default: break;
        }
    } else if (was_moving) {
        offset_committed_selection_boxes(0);

        const auto& src = curr_source_.event_source;
        if (src == EventSourceKind::Velocity) {
            util::Debugger::log_error("Cant move note velocity points.");
            return;
        }

        const auto& nav = navigation()->value;
        const midi::MIDITick mouse_tick =
            static_cast<midi::MIDITick>(std::llround(mouse_pos_.x * nav.zoom_ticks_smoothed + nav.tick_pos_smoothed));
        const midi::MIDITick mouse_tick_snapped = _controller->get_project_manager()->get_timeline()->snap_nearest(mouse_tick);
        const SignedMIDITick delta_ticks =
            static_cast<SignedMIDITick>(mouse_tick_snapped) - static_cast<SignedMIDITick>(mouse_tick_anchor_);

        const std::uint16_t active_track = _controller->get_active_track();

        switch (src) {
        case EventSourceKind::Tempo: {
            with_selection(global_metas(), [&]() { return _controller->get_meta_event_selection(); },
                history::kGlobalTrack,
                [&](const std::vector<midi::MetaEvent>& metas, const std::vector<std::size_t>& ids) {
                    const bool has_unmoved_tempo_at_zero = std::find_if(
                        metas.begin(), metas.end(),
                        [&](const midi::MetaEvent& event) {
                            if (event.tick != 0 ||
                                event.event_type != midi::MetaEventType::Tempo) {
                                return false;
                            }

                            const auto id = static_cast<std::size_t>(&event - metas.data());

                            return std::ranges::find(ids, id) == ids.end();
                        }
                    ) != metas.end();

                    const bool moves_tempo_to_zero = std::find_if(
                        ids.begin(), ids.end(),
                        [&](std::size_t id) {
                            const auto& event = metas[id];

                            return event.event_type == midi::MetaEventType::Tempo &&
                                static_cast<SignedMIDITick>(event.tick) + delta_ticks == 0;
                        }
                    ) != ids.end();

                    auto tx = _controller->begin_action("Move tempo events");
                    auto meta_mutator = tx.global_meta_events(ids);
                    meta_mutator.move_events(ids, delta_ticks);

                    if (!has_unmoved_tempo_at_zero && !moves_tempo_to_zero) {
                        meta_mutator.add_event(DEFAULT_TEMPO);
                    }

                    auto sel_mutator = tx.selection();
                    sel_mutator.meta_events().remap(meta_mutator);

                    if (tx.commit()) {
                        translate_committed_selection_boxes(delta_ticks);
                        bump_revision();
                    }
                });
            break;
        }
        case EventSourceKind::PitchBend:
        case EventSourceKind::ProgramChange:
        case EventSourceKind::ControlChange:
        case EventSourceKind::ChannelPressure:
        case EventSourceKind::PolyPressure: {
            with_selection(track_events(active_track), [&]() { return _controller->get_channel_event_selection(); },
                active_track,
                [&](const std::vector<midi::ChannelEvent>& pitch_bends, const std::vector<std::size_t>& ids) {
                    auto tx = _controller->begin_action("Move channel events");
                    auto event_mutator = tx.channel_events(active_track, ids);
                    event_mutator.move_events(ids, delta_ticks);

                    auto sel_mutator = tx.selection();
                    sel_mutator.channel_events().remap(event_mutator);

                    if (tx.commit()) {
                        translate_committed_selection_boxes(delta_ticks);
                        bump_revision();
                    }
                });
            break;
        }
        default: {
            break;
        }
        }
    }
}

void DataEditing::copy_selected_events() {
    const auto& src = curr_source_.event_source;
    const std::uint16_t active_track = _controller->get_active_track();

    switch (src) {
    case EventSourceKind::Tempo: {
        with_selection(global_metas(), [&]() { return _controller->get_meta_event_selection(); },
            history::kGlobalTrack, [&](const std::vector<midi::MetaEvent>& events, const std::vector<std::size_t>& ids) {
                std::vector<midi::MetaEvent> copied_events;
                copied_events.reserve(ids.size());
                for (const std::size_t id : ids) {
                    copied_events.push_back(events[id]);
                }
                clipboard_.events_to_clipboard(std::move(copied_events), curr_source_);
            });
        break;
    }
    case EventSourceKind::PitchBend:
    case EventSourceKind::ControlChange:
    case EventSourceKind::ProgramChange:
    case EventSourceKind::ChannelPressure:
    case EventSourceKind::PolyPressure: {
        with_selection(track_events(active_track), [&]() { return _controller->get_channel_event_selection(); },
            active_track, [&](const std::vector<midi::ChannelEvent>& events, const std::vector<std::size_t>& ids) {
                std::vector<midi::ChannelEvent> copied_events;
                copied_events.reserve(ids.size());
                for (const std::size_t id : ids) {
                    copied_events.push_back(events[id]);
                }
                clipboard_.events_to_clipboard(std::move(copied_events), curr_source_);
            });
        break;
    }
    }
}

void DataEditing::paste_selected_events() {
    const auto& src = curr_source_.event_source;
    const std::uint16_t active_track = _controller->get_active_track();

    const auto* copied_source = clipboard_.get_copied_source();
    if (!copied_source) return;

    const auto [src_min, src_max] = EventSources::get_source(copied_source->event_source)->info.range;
    const auto [dst_min, dst_max] = EventSources::get_source(src)->info.range;

    const auto remap_value = [src_min, src_max, dst_min, dst_max](double value) {
        if (src_min == src_max) return dst_min;
        if (src_min == dst_min && src_max == dst_max) return value;

        const double t = (value - src_min) / (src_max - src_min);
        return t * (dst_max - dst_min) + dst_min;
    };

    const auto convert_event = [&](const midi::ChannelEvent& event) -> std::optional<midi::ChannelEvent> {
        std::optional<double> mapped_value = std::visit([&](auto& v) -> std::optional<double> {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_same_v<T, midi::PitchBend>) {
                const int raw = (static_cast<int>(v.msb) << 7) | v.lsb;
                const int decoded = raw - 8192;
                return static_cast<double>(std::lround(std::clamp(remap_value(decoded), dst_min, dst_max)));
            }
            else if constexpr (std::is_same_v<T, midi::Controller>) {
                return static_cast<double>(std::lround(std::clamp(remap_value(v.value), dst_min, dst_max)));
            }
            else if constexpr (std::is_same_v<T, midi::ChannelAftertouch>) {
                return static_cast<double>(std::lround(std::clamp(remap_value(v.amount), dst_min, dst_max)));
            }
            else if constexpr (std::is_same_v<T, midi::NoteAftertouch>) {
                return static_cast<double>(std::lround(std::clamp(remap_value(v.pressure), dst_min, dst_max)));
            }
            else if constexpr (std::is_same_v<T, midi::ProgramChange>) {
                return std::nullopt;
            }

            return std::nullopt;
        }, event.event_type);

        if (mapped_value.has_value()) {
            double val = *mapped_value;

            const std::uint8_t channel = curr_source_.channel == 0xFF
                ? event.channel
                : curr_source_.channel;

            switch (src) {
                case EventSourceKind::PitchBend: {
                    const int raw = static_cast<int>(val) + 8192;
                    return midi::ChannelEvent{
                        .tick = event.tick,
                        .channel = channel,
                        .event_type = midi::PitchBend{ .lsb = static_cast<std::uint8_t>(raw & 0x7F),
                                                       .msb = static_cast<std::uint8_t>((raw >> 7) & 0x7F) }
                    };
                }
                case EventSourceKind::ControlChange: {
                    return midi::ChannelEvent{
                        .tick = event.tick,
                        .channel = channel,
                        .event_type = midi::Controller { .controller = curr_source_.controller,
                                                         .value = static_cast<std::uint8_t>(val) }
                    };
                }
                default: {
                    // TODO: other events
                    break;
                }
            }
        }

        return std::nullopt;
    };


    switch (src) {
    case EventSourceKind::Tempo: {
        const auto* copied_tempos = clipboard_.get_copied_metas();
        if (!copied_tempos) return;
        if (copied_tempos->empty()) return;

        midi::MIDITick first_tick = copied_tempos->front().tick;
        midi::MIDITick playhead_tick = editor_context()->playback->playback_start_tick();

        auto tx = _controller->begin_action("Paste tempo events");
        auto metas = tx.global_meta_events({});

        std::vector<history::AddedMetaEvent> tickets{};
        tickets.reserve(copied_tempos->size());
        for (midi::MetaEvent tempo : *copied_tempos) {
            tempo.tick = tempo.tick - first_tick + playhead_tick;
            tickets.push_back(metas.add_event(std::move(tempo)));
        }

        auto selection = tx.selection().meta_events();
        std::vector<std::size_t> final_ids{};
        final_ids.reserve(copied_tempos->size());
        for (const auto& ticket : tickets) {
            final_ids.push_back(metas.final_id(ticket));
        }

        selection.set(history::kGlobalTrack, final_ids);

        if (tx.commit()) {
            bump_revision();
        }
        break;
    }
    case EventSourceKind::PitchBend:
    case EventSourceKind::ControlChange:
    case EventSourceKind::ProgramChange:
    case EventSourceKind::ChannelPressure:
    case EventSourceKind::PolyPressure: {
        const auto* copied_events = clipboard_.get_copied_events();
        if (!copied_events) return;
        if (copied_events->empty()) return;

        midi::MIDITick first_tick = copied_events->front().tick;
        midi::MIDITick playhead_tick = editor_context()->playback->playback_start_tick();

        auto tx = _controller->begin_action("Paste events");
        auto events = tx.channel_events(active_track, {});

        std::vector<history::AddedChannelEvent> tickets{};
        tickets.reserve(copied_events->size());
        bool has_converted = false;
        for (const auto& event : *copied_events) {
            std::optional<midi::ChannelEvent> converted = convert_event(event);
            if (!converted) continue;

            converted->tick = converted->tick - first_tick + playhead_tick;
            tickets.push_back(events.add_event(std::move(*converted)));
            has_converted = true;
        }

        if (has_converted) {
            auto selection = tx.selection().channel_events();
            std::vector<std::size_t> final_ids{};
            final_ids.reserve(copied_events->size());
            for (const auto& ticket : tickets) {
                final_ids.push_back(events.final_id(ticket));
            }

            selection.set(active_track, final_ids);
        }

        if (tx.commit()) {
            bump_revision();
        }
        break;
    }
    }
}

DataEditing::SelectionResult DataEditing::select_events_in_between(MIDITick min_tick, MIDITick max_tick,
    std::optional<EventSourceKind> source_override) {
    SelectionResult result{};

    const auto& src = source_override ? *source_override : curr_source_.event_source;
    const std::uint16_t active_track = _controller->get_active_track();

    auto select_from = [&result, min_tick, max_tick](const auto* events, auto tick_getter, auto predicate) {
        if (!events) return;
        result.ids = select_ranged(*events, tick_getter, min_tick, max_tick, predicate);
    };

    auto select_channel_events = [&](auto predicate) {
        select_from(track_events(active_track),
            [](const midi::ChannelEvent& event) { return event.tick; },
            [&](const midi::ChannelEvent& event) {
                if (curr_source_.channel != 0xFF && curr_source_.channel != event.channel)
                    return false;
                return predicate(event);
            });
    };

    switch (src) {
        case EventSourceKind::Velocity: {
            select_from(track_notes(active_track),
                [](const midi::Note& note) { return note.get_start(); },
                [](const auto&) { return true; });
            break;
        }
        case EventSourceKind::Tempo: {
            select_from(global_metas(),
                [](const midi::MetaEvent& meta) { return meta.tick; },
                [](const midi::MetaEvent& meta) { return meta.event_type == midi::MetaEventType::Tempo; });
            break;
        }
        case EventSourceKind::PitchBend: {
            select_channel_events([](const midi::ChannelEvent& event) {
                return std::holds_alternative<midi::PitchBend>(event.event_type);
            });
            break;
        }
        case EventSourceKind::ProgramChange: {
            select_channel_events([](const midi::ChannelEvent& event) {
                return std::holds_alternative<midi::ProgramChange>(event.event_type);
            });
            break;
        }
        case EventSourceKind::ControlChange: {
            select_channel_events([&](const midi::ChannelEvent& event) {
                return std::holds_alternative<midi::Controller>(event.event_type) &&
                    std::get<midi::Controller>(event.event_type).controller == curr_source_.controller;
            });
            break;
        }
        case EventSourceKind::ChannelPressure: {
            select_channel_events([](const midi::ChannelEvent& event) {
                return std::holds_alternative<midi::ChannelAftertouch>(event.event_type);
                });
            break;
        }
        // TODO: Key pressure lane.
        default: break;
    }
    result.have = !result.ids.empty();

    return result;
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

#pragma region editing
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
        const auto& events = tracks->at(curr_track).get_channel_evs();

        std::vector<std::size_t> matching_ids = select_ranged(events,
            [](const midi::ChannelEvent& event) { return event.tick; },
            tick, tick,
            [&](const midi::ChannelEvent& event) {
                if (!std::holds_alternative<midi::ProgramChange>(event.event_type)) return false;
                if (curr_source_.channel != 0xFF && curr_source_.channel != event.channel) return false;
                return true;
            }, true);

        std::array<bool, 16> channels_present{};
        for (const std::size_t& id : matching_ids) {
            const auto& event = events[id];
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

void DataEditing::delete_selected_events() {
    const auto& src = curr_source_.event_source;
    const std::uint16_t active_track = _controller->get_active_track();

    switch (src) {
    case EventSourceKind::Velocity: {
        return;
    }
    case EventSourceKind::Tempo: {
        // TODO: move this to "with source selection" function

        with_selection(global_metas(), [&]() { return _controller->get_meta_event_selection(); },
            history::kGlobalTrack,
            [&](const std::vector<midi::MetaEvent>& tempos, const std::vector<std::size_t>& ids) {
                const bool deletes_first_tempo = ids.at(0) != 0;

                auto tx = _controller->begin_action("Delete tempo events");
                auto mutator = tx.global_meta_events(ids);
                if (deletes_first_tempo) {
                    mutator.add_event(DEFAULT_TEMPO);
                }

                mutator.delete_events(ids);

                auto sel = tx.selection().meta_events();
                sel.remap(mutator);

                if (tx.commit()) {
                    clear_committed_selection_boxes();
                    bump_revision();
                }
            });

        break;
    }
    case EventSourceKind::PitchBend: 
    case EventSourceKind::ProgramChange:
    case EventSourceKind::ControlChange:
    case EventSourceKind::ChannelPressure:
    case EventSourceKind::PolyPressure: {
        with_selection(track_events(active_track), [&]() { return _controller->get_channel_event_selection(); },
            active_track,
            [&](const std::vector<midi::ChannelEvent>& events, const std::vector<std::size_t>& ids) {
                auto tx = _controller->begin_action("Delete events");
                auto mutator = tx.channel_events(active_track, ids);
                mutator.delete_events(ids);
                tx.selection().channel_events().remap(mutator);
                if (tx.commit()) {
                    clear_committed_selection_boxes();
                    bump_revision();
                }
            });
        break;
    }
    default:
        // TODO: other lanes
        break;
    }
}

#pragma endregion

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

void DataEditing::offset_committed_selection_boxes(SignedMIDITick tick) {
    for (auto* box : committed_selection_boxes) box->info.offset.x = tick;
}

void DataEditing::translate_committed_selection_boxes(SignedMIDITick tick) {
    for (auto* box : committed_selection_boxes) {
        auto& i = box->info;
        i.start.x += tick;
        i.end.x += tick;
        i.offset.x = 0;
    }
}

#pragma endregion

#pragma endregion

}
