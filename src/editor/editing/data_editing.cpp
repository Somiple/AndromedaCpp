#include "editor/editing/data_editing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include "editor/editor_controller.h"
#include "editor/project/project_manager.h"
#include "ui/app/main_window.h"
#include "core/debugger.h"
#include "core/collections/generic.h"
#include "core/util.h"

namespace {

using andromeda::midi::MIDITick;
using namespace andromeda::util::collections::generic;

struct RampSpec {
    MIDITick min_tick;
    MIDITick max_tick;
    bool snap;
    double tick_interval;
};

/// @return position of @p t along the ramp, 0..1 (0 for a zero-width ramp).
inline double ramp_factor(MIDITick t, MIDITick min_tick, MIDITick max_tick) {
    // a zero-width drag would otherwise divide by zero
    return max_tick > min_tick
        ? static_cast<double>(t - min_tick) / static_cast<double>(max_tick - min_tick)
        : 0.0;
}

/// Replaces the matching events inside the ramp and adds new ones along it.
///   predicate(event)            -> does the event belong to the edited source?
///   make_mutator(ids)           -> mutator for the matching events
///   convert_value(v)            -> value exactly as it will be stored (used to skip redundant steps)
///   value_at(x)                 -> ramp value at x (0..1)
///   replacer(mutator, id, v)    -> rewrite an existing event
///   adder(mutator, tick, v)     -> add a new event
template <typename Events, typename Predicate, typename MakeMutator, typename Convert,
    typename ValueAt, typename Replace, typename Add>
void add_and_replace_in_between(Events& events, const RampSpec& ramp, Predicate predicate,
    MakeMutator make_mutator, Convert convert_value,
    ValueAt value_at, Replace replacer, Add adder) {
    const auto [min_tick, max_tick, snap, tick_interval] = ramp;
    if (max_tick < min_tick) return;

    const auto factor_at = [&](MIDITick t) { return ramp_factor(t, min_tick, max_tick); };

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

#pragma region helpers

namespace {

constexpr std::uint8_t ALL_CHANNELS = 0xFF;
constexpr long PITCH_BEND_CENTER = 8192; // raw 14-bit value 0..16383 sits around this

/// @return the track at @p track, or nullptr when the controller, project or track is unavailable.
midi::MIDITrack* find_track(EditorController* controller, std::uint16_t track) {
    if (controller == nullptr) return nullptr;

    ProjectManager* manager = controller->get_project_manager();
    if (manager == nullptr) return nullptr;

    std::vector<midi::MIDITrack>* tracks = manager->get_tracks();
    if (tracks == nullptr || track >= tracks->size()) return nullptr;

    return &(*tracks)[track];
}

/// @return true for the sources that are stored as channel events of a track.
bool is_channel_event_source(EventSourceKind kind) {
    switch (kind) {
    case EventSourceKind::PitchBend:
    case EventSourceKind::ProgramChange:
    case EventSourceKind::ControlChange:
    case EventSourceKind::ChannelPressure:
    case EventSourceKind::PolyPressure:
        return true;
    default:
        return false;
    }
}

bool is_tempo(const midi::MetaEvent& event) {
    return event.event_type == midi::MetaEventType::Tempo;
}

/// The single definition of "does this channel event belong to the source being edited?":
/// channel filter, event type, and the controller / key the source is pinned to.
bool event_matches_source(const DataEditSource& source, const midi::ChannelEvent& event) {
    if (source.channel != ALL_CHANNELS && source.channel != event.channel) return false;

    const auto& type = event.event_type;
    switch (source.event_source) {
    case EventSourceKind::PitchBend:
        return std::holds_alternative<midi::PitchBend>(type);
    case EventSourceKind::ProgramChange:
        return std::holds_alternative<midi::ProgramChange>(type);
    case EventSourceKind::ChannelPressure:
        return std::holds_alternative<midi::ChannelAftertouch>(type);
    case EventSourceKind::ControlChange: {
        const auto* cc = std::get_if<midi::Controller>(&type);
        return cc != nullptr && cc->controller == source.controller;
    }
    case EventSourceKind::PolyPressure: {
        const auto* pp = std::get_if<midi::NoteAftertouch>(&type);
        return pp != nullptr && pp->key == source.key;
    }
    default:
        return false;
    }
}

/// @return the event's value in the source's own units (pitch bend is centered on 0), or
///         nullopt for events that carry no value.
std::optional<double> event_value(const midi::ChannelEvent& event) {
    return std::visit([](const auto& v) -> std::optional<double> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, midi::PitchBend>) {
            return static_cast<double>(((static_cast<int>(v.msb) << 7) | v.lsb) - PITCH_BEND_CENTER);
        } else if constexpr (std::is_same_v<T, midi::Controller>) {
            return static_cast<double>(v.value);
        } else if constexpr (std::is_same_v<T, midi::ChannelAftertouch>) {
            return static_cast<double>(v.amount);
        } else if constexpr (std::is_same_v<T, midi::NoteAftertouch>) {
            return static_cast<double>(v.pressure);
        } else if constexpr (std::is_same_v<T, midi::ProgramChange>) {
            return static_cast<double>(v.program);
        } else {
            return std::nullopt;
        }
    }, event.event_type);
}

/// @return @p value rounded and clamped to what the source can actually store.
long coerce_to_source(EventSourceKind kind, double value) {
    const auto [min, max] = EventSources::get_source(kind)->info.range;
    long lo = std::lround(min);
    long hi = std::lround(max);
    if (kind == EventSourceKind::PitchBend) {
        // a range that reaches +8192 would overflow the 14-bit raw value into the msb and wrap
        // to the lowest bend
        lo = std::max(lo, -PITCH_BEND_CENTER);
        hi = std::min(hi, PITCH_BEND_CENTER - 1);
    }
    // clamp in a wide type first: casting to uint8 before clamping wrapped out-of-range values
    return std::clamp(std::lround(value), lo, hi);
}

/// Builds the event the source stores for @p value; the one place a value becomes an event.
std::optional<midi::ChannelEvent> make_channel_event(const DataEditSource& source,
    midi::MIDITick tick, std::uint8_t channel, double value) {
    const long coerced = coerce_to_source(source.event_source, value);
    const auto u8 = static_cast<std::uint8_t>(coerced);

    const auto make = [&](auto payload) {
        return midi::ChannelEvent{ .tick = tick, .channel = channel, .event_type = std::move(payload) };
    };

    switch (source.event_source) {
    case EventSourceKind::PitchBend: {
        const auto raw = static_cast<std::uint16_t>(coerced + PITCH_BEND_CENTER);
        return make(midi::PitchBend{ .lsb = static_cast<std::uint8_t>(raw & 0x7F),
                                     .msb = static_cast<std::uint8_t>((raw >> 7) & 0x7F) });
    }
    case EventSourceKind::ControlChange:
        return make(midi::Controller{ .controller = source.controller, .value = u8 });
    case EventSourceKind::ChannelPressure:
        return make(midi::ChannelAftertouch{ .amount = u8 });
    case EventSourceKind::PolyPressure:
        return make(midi::NoteAftertouch{ .key = source.key, .pressure = u8 });
    case EventSourceKind::ProgramChange:
        return make(midi::ProgramChange{ .program = u8 });
    default:
        return std::nullopt;
    }
}

/// Rewrites the value of an existing event of the source.
void set_event_value(history::ChannelEventMutator& mutator, const DataEditSource& source,
    std::size_t id, long value) {
    const auto u8 = static_cast<std::uint8_t>(value);
    switch (source.event_source) {
    case EventSourceKind::PitchBend:
        mutator.set_pitch_bend(id, static_cast<std::uint16_t>(value + PITCH_BEND_CENTER));
        break;
    case EventSourceKind::ControlChange:
        mutator.set_control_change(id, source.controller, u8);
        break;
    case EventSourceKind::ChannelPressure:
        mutator.set_channel_pressure(id, u8);
        break;
    case EventSourceKind::PolyPressure:
        mutator.set_poly_pressure_value(id, u8);
        break;
    default:
        break;
    }
}

const char* set_action_label(EventSourceKind kind) {
    switch (kind) {
    case EventSourceKind::PitchBend:       return "Set pitch bend";
    case EventSourceKind::ControlChange:   return "Set control change value";
    case EventSourceKind::ChannelPressure: return "Set channel pressure value";
    case EventSourceKind::PolyPressure:    return "Set key pressure value";
    default:                               return "Set value";
    }
}

/// Calls @p fn with every channel the source writes to: all 16, or just the selected one.
template <typename Fn>
void for_each_target_channel(const DataEditSource& source, Fn&& fn) {
    if (source.channel == ALL_CHANNELS) {
        for (std::uint8_t channel = 0; channel < 16; channel++) fn(channel);
    } else {
        fn(static_cast<std::uint8_t>(source.channel & 0x0F));
    }
}

void add_event_on_channels(history::ChannelEventMutator& mutator, const DataEditSource& source,
    midi::MIDITick tick, double value) {
    for_each_target_channel(source, [&](std::uint8_t channel) {
        if (auto event = make_channel_event(source, tick, channel, value)) {
            mutator.add_event(std::move(*event));
        }
    });
}

// @param delta how far the selected events move; nullopt when they’re removed.
bool tempo_remains_at_zero(const std::vector<midi::MetaEvent>& metas,
    const std::vector<std::size_t>& ids, std::optional<SignedMIDITick> delta) {
    
    std::size_t at_zero = 0;
    for (const auto& meta : metas) {
        if (is_tempo(meta) && meta.tick == 0) at_zero++;
    }

    std::size_t selected_at_zero = 0;
    for (const std::size_t id : ids) {
        const auto& meta = metas[id];
        if (!is_tempo(meta)) continue;
        if (meta.tick == 0) selected_at_zero++;
        if (delta && static_cast<SignedMIDITick>(meta.tick) + *delta == 0) return true;
    }

    return at_zero > selected_at_zero;
}

// @param ids the ids waiting to be deleted
bool tempo_kept_at_zero(const std::vector<midi::MetaEvent>& metas, const std::vector<std::size_t>& ids) {
    return tempo_remains_at_zero(metas, ids, std::nullopt);
}

/// @return the box's tick span as (low, high); a box dragged right-to-left has its end before its start.
template <typename Box>
std::pair<std::int64_t, std::int64_t> tick_span(Box& box) {
    const auto bounds = box.bounds();
    const auto a = static_cast<std::int64_t>(bounds.first.x);
    const auto b = static_cast<std::int64_t>(bounds.second.x);
    return { std::min(a, b), std::max(a, b) };
}

/// Maps a position normalized to the work rect onto the screen.
template <typename Rect>
Vector2<float> norm_to_screen(const Rect& rc, double x_norm, double y_norm) {
    return Vector2<float>(static_cast<float>(x_norm * rc.width + rc.left),
                          static_cast<float>(y_norm * rc.height + rc.top));
}

/// @param x position along the ramp, 0..1.
/// @return the weight used to mix the ramp's two endpoints with the tool's curve.
double ramp_weight(const DataEditTool& tool, double x) {
    const bool is_linear = tool.tool_kind == DataEditToolKind::Line ||
        tool.curve_kind == DataEditCurveKind::Linear;
    return is_linear ? x : DataEditing::curve_weight(tool.curve_kind, x, tool.influence);
}

/// Runs @p on_metas for the tempo selection or @p on_events for the selected channel events of
/// @p track, depending on the source. Other sources have nothing selectable here.
template <typename OnMetas, typename OnEvents>
void with_source_selection(const DataEditing& editor, EditorController* controller,
    EventSourceKind src, std::uint16_t track, OnMetas&& on_metas, OnEvents&& on_events) {
    if (src == EventSourceKind::Tempo) {
        with_selection(editor.global_metas(), [&]() { return controller->get_meta_event_selection(); },
            history::kGlobalTrack, on_metas);
    } else if (is_channel_event_source(src)) {
        with_selection(editor.track_events(track), [&]() { return controller->get_channel_event_selection(); },
            track, on_events);
    }
}

} // namespace

#pragma endregion

#pragma region Data clipboard

std::vector<midi::MetaEvent>* DataEditClipboard::get_copied_metas() {
    return std::get_if<std::vector<midi::MetaEvent>>(&copied_events_);
}

std::vector<midi::ChannelEvent>* DataEditClipboard::get_copied_events() {
    return std::get_if<std::vector<midi::ChannelEvent>>(&copied_events_);
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
    midi::MIDITrack* t = find_track(_controller, track);
    return t ? &t->get_notes() : nullptr;
}

const std::vector<midi::ChannelEvent>* DataEditing::track_events(std::uint16_t track) const {
    midi::MIDITrack* t = find_track(_controller, track);
    return t ? &t->get_channel_evs() : nullptr;
}

const std::vector<midi::MetaEvent>* DataEditing::global_metas() const {
    if (_controller == nullptr) return nullptr;

    ProjectManager* manager = _controller->get_project_manager();
    return manager ? manager->get_metas() : nullptr;
}

void DataEditing::update() {
    if (!_overlay_drawer)
        _overlay_drawer = std::make_unique<overlays::OverlayDrawer>(&context());

    const auto& work_rect = context().rect;
    const auto& mouse_screen = *mouse_pos();

    // a collapsed rect (panel hidden or mid-resize) would divide by zero below
    if (work_rect.width <= 0.0f || work_rect.height <= 0.0f) {
        stats_.stats_present = false;
        return;
    }

    // map the mouse to the work rect, clamping to the rect's bounds
    mouse_pos_.x = std::clamp((mouse_screen.x - work_rect.left) / work_rect.width, 0.0f, 1.0f);
    mouse_pos_.y = std::clamp((mouse_screen.y - work_rect.top) / work_rect.height, 0.0f, 1.0f);
    update_stats();
}

void DataEditing::cleanup() {
    cancel_gesture();
    clear_committed_selection_boxes();
}

void DataEditing::cancel_gesture() {
    disable_flag(DATA_EDIT_DRAW_EDIT_LINE | DATA_EDIT_MOUSE_DOWN_IN_SEL | DATA_EDIT_CLICKED_IN_RECT);
    m_active_gesture = SelectionGesture::Replace;
    if (live_selection_box) end_live_selection_box();
    offset_committed_selection_boxes(0);
}

midi::MIDITick DataEditing::tick_at_norm_x(double x_norm) const {
    const auto& nav = navigation()->value;
    const double tick = x_norm * static_cast<double>(nav.zoom_ticks_smoothed) +
                        static_cast<double>(nav.tick_pos_smoothed);
    // smooth scrolling can park the view before tick 0; a negative double cast to an unsigned
    // tick is undefined behavior and in practice a huge tick
    return static_cast<midi::MIDITick>(std::llround(std::max(0.0, tick)));
}

midi::MIDITick DataEditing::snapped_mouse_tick() const {
    const midi::MIDITick tick = tick_at_norm_x(mouse_pos_.x);
    return _controller->get_project_manager()->get_timeline()->snap_nearest(tick);
}

// Pencil and Eraser are not implemented yet (TODO: live-edit the closest clicked data point:
// velocity, pitch-bend, cc event, poly), so they do nothing in the three handlers below.

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

    switch (tool_.tool_kind) {
    case DataEditToolKind::Line:
    case DataEditToolKind::Curve:
        // instrument changes are discrete; a ramp over them is meaningless (and only logs an error)
        if (curr_source_.event_source == EventSourceKind::ProgramChange) return;
        pencil_mouse_down();
        break;
    case DataEditToolKind::Selector:
        select_mouse_down();
        break;
    case DataEditToolKind::Pencil:
        break;
    case DataEditToolKind::Eraser:
        eraser_mouse_down();
        break;
    }

    enable_flag(DATA_EDIT_CLICKED_IN_RECT);
}

void DataEditing::on_mouse_move() {
    // the ramp tools only act on release
    if (tool_.tool_kind == DataEditToolKind::Selector) {
        select_mouse_move();
    } else if (tool_.tool_kind == DataEditToolKind::Eraser) {
        eraser_mouse_move();
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

    // released over other UI or under a dialog: abandon the gesture instead of leaving the edit
    // line / live selection box stuck on screen forever
    if (get_flag(DATA_EDIT_MOUSE_OVER_UI | DATA_EDIT_ANY_DIALOG_OPEN)) {
        cancel_gesture();
        return;
    }

    switch (tool_.tool_kind) {
    case DataEditToolKind::Line:
    case DataEditToolKind::Curve:
        pencil_mouse_up();
        break;
    case DataEditToolKind::Selector:
        select_mouse_up();
        break;
    case DataEditToolKind::Pencil:
        break;
    case DataEditToolKind::Eraser:
        eraser_mouse_up();
        break;
    }
}

void DataEditing::on_key_down(const KeyState& keys) {
    // cut is copy + delete; it used to be declared in KeyState and then ignored
    if (keys.copy || keys.cut) {
        copy_selected_events();
    }

    if (keys.del || keys.cut) {
        delete_selected_events();
    }

    if (keys.paste) {
        paste_selected_events();
    }
}

void DataEditing::pencil_mouse_down() {
    mouse_anchor_ = mouse_pos_;
    enable_flag(DATA_EDIT_DRAW_EDIT_LINE);
}

void DataEditing::pencil_mouse_up() {
    disable_flag(DATA_EDIT_DRAW_EDIT_LINE);

    // the point's x and y are normalized to the work rect; map them to the data space
    const auto map_point = [&](Vector2<float> point) {
        return std::pair{ tick_at_norm_x(point.x), 1.0 - static_cast<double>(point.y) };
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

    const auto selection_type = m_active_gesture == SelectionGesture::Subtract
        ? overlays::SelectionType::RemoveSelect
        : overlays::SelectionType::AddSelect;

    const midi::MIDITick mouse_tick = tick_at_norm_x(mouse_pos_.x);
    const midi::MIDITick mouse_tick_snapped = snapped_mouse_tick();

    const bool inside_committed_box = std::ranges::any_of(committed_selection_boxes, [&](auto* box) {
        const auto [lo, hi] = tick_span(*box);
        const auto tick = static_cast<std::int64_t>(mouse_tick);
        return tick >= lo && tick <= hi;
    });

    if (inside_committed_box && source_is_mutable()) {
        enable_flag(DATA_EDIT_MOUSE_DOWN_IN_SEL);
        mouse_tick_anchor_ = mouse_tick_snapped;
        return;
    }

    if (!live_selection_box) {
        begin_live_selection_box(mouse_tick_snapped, selection_type);
    }
}

void DataEditing::select_mouse_move() {
    const midi::MIDITick mouse_tick_snapped = snapped_mouse_tick();

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

    const bool was_moving = get_flag(DATA_EDIT_MOUSE_DOWN_IN_SEL);
    disable_flag(DATA_EDIT_MOUSE_DOWN_IN_SEL);

    if (live_selection_box) {
        // select_ranged needs min <= max; dragging right-to-left used to select nothing (and
        // clear the selection on Replace)
        const auto [lo, hi] = tick_span(*live_selection_box);
        end_live_selection_box();

        apply_selection_gesture(gesture,
            static_cast<midi::MIDITick>(std::max<std::int64_t>(0, lo)),
            static_cast<midi::MIDITick>(std::max<std::int64_t>(0, hi)));
    } else if (was_moving) {
        offset_committed_selection_boxes(0);
        move_selected_events();
    }
}

void DataEditing::eraser_mouse_down() {
    if (!source_is_mutable()) return; // ignores note events
    begin_live_selection_box(snapped_mouse_tick(), overlays::SelectionType::Delete);
}

void DataEditing::eraser_mouse_move() {
    if (!live_selection_box) return;
    update_live_selection_box(snapped_mouse_tick());
}

void DataEditing::eraser_mouse_up() {
    if (!live_selection_box) return;

    const auto [lo, hi] = tick_span(*live_selection_box);
    end_live_selection_box();

    delete_events_by_ids(select_events_in_between(
        static_cast<midi::MIDITick>(std::max<std::int64_t>(0, lo)),
        static_cast<midi::MIDITick>(std::max<std::int64_t>(0, hi))).ids);
}

void DataEditing::apply_selection_gesture(SelectionGesture gesture, MIDITick min_tick, MIDITick max_tick) {
    const EventSourceKind src = curr_source_.event_source;
    const std::uint16_t track = lane_track();
    const std::vector<std::size_t> ids = select_events_in_between(min_tick, max_tick).ids;

    const auto apply = [&](auto&& get_selection, auto selection_track) {
        // adding or removing nothing is a no-op: no undo entry, no phantom committed box
        if (ids.empty() && gesture != SelectionGesture::Replace) return;

        auto tx = _controller->begin_action("Select/deselect");
        auto selection = get_selection(tx);

        if (gesture == SelectionGesture::Replace) {
            clear_committed_selection_boxes();
            if (ids.empty()) {
                selection.clear_track(selection_track);
            } else {
                selection.set(selection_track, ids);
            }
        } else if (gesture == SelectionGesture::Add) {
            selection.add(selection_track, ids);
        } else if (gesture == SelectionGesture::Subtract) {
            selection.remove(selection_track, ids);
        }

        if (!ids.empty() && gesture != SelectionGesture::Subtract) {
            add_committed_selection_box({ min_tick, max_tick });
        }

        tx.commit();
    };

    if (src == EventSourceKind::Velocity) {
        apply([](history::Transaction& tx) { return tx.selection(); }, track);
    } else if (src == EventSourceKind::Tempo) {
        apply([](history::Transaction& tx) { return tx.selection().meta_events(); }, history::kGlobalTrack);
    } else if (is_channel_event_source(src)) {
        apply([](history::Transaction& tx) { return tx.selection().channel_events(); }, track);
    }
}

void DataEditing::move_selected_events() {
    // note velocities stay with their notes
    if (!source_is_mutable()) return;

    SignedMIDITick delta_ticks =
        static_cast<SignedMIDITick>(snapped_mouse_tick()) - static_cast<SignedMIDITick>(mouse_tick_anchor_);

    // a click without a drag is not a move; skip the empty undo entry
    if (delta_ticks == 0) return;

    const std::uint16_t track = lane_track();

    // never push an event before tick 0: ticks are unsigned, so it would wrap to a huge value
    const auto clamp_delta_to_start = [&delta_ticks](const auto& events, const std::vector<std::size_t>& ids) {
        if (delta_ticks >= 0 || ids.empty()) return;
        SignedMIDITick earliest = std::numeric_limits<SignedMIDITick>::max();
        for (const std::size_t id : ids) {
            earliest = std::min(earliest, static_cast<SignedMIDITick>(events[id].tick));
        }
        delta_ticks = std::max(delta_ticks, -earliest);
    };

    with_source_selection(*this, _controller, curr_source_.event_source, track,
        [&](const std::vector<midi::MetaEvent>& metas, const std::vector<std::size_t>& ids) {
            clamp_delta_to_start(metas, ids);
            if (delta_ticks == 0) return;

            const bool keeps_tempo_at_zero = tempo_remains_at_zero(metas, ids, delta_ticks);

            auto tx = _controller->begin_action("Move tempo events");
            auto mutator = tx.global_meta_events(ids);
            mutator.move_events(ids, delta_ticks);
            if (!keeps_tempo_at_zero) {
                mutator.add_event(DEFAULT_TEMPO);
            }

            auto selection = tx.selection().meta_events();
            selection.remap(mutator);

            if (commit_and_bump(tx)) translate_committed_selection_boxes(delta_ticks);
        },
        [&](const std::vector<midi::ChannelEvent>& events, const std::vector<std::size_t>& ids) {
            clamp_delta_to_start(events, ids);
            if (delta_ticks == 0) return;

            auto tx = _controller->begin_action("Move channel events");
            auto mutator = tx.channel_events(track, ids);
            mutator.move_events(ids, delta_ticks);

            auto selection = tx.selection().channel_events();
            selection.remap(mutator);

            if (commit_and_bump(tx)) translate_committed_selection_boxes(delta_ticks);
        });
}

void DataEditing::copy_selected_events() {
    const auto copy = [this](const auto& events, const std::vector<std::size_t>& ids) {
        using Event = typename std::decay_t<decltype(events)>::value_type;
        std::vector<Event> copied;
        copied.reserve(ids.size());
        for (const std::size_t id : ids) {
            copied.push_back(events[id]);
        }
        clipboard_.events_to_clipboard(std::move(copied), curr_source_);
    };

    with_source_selection(*this, _controller, curr_source_.event_source, lane_track(), copy, copy);
}

void DataEditing::paste_selected_events() {
    const EventSourceKind src = curr_source_.event_source;
    const std::uint16_t track = lane_track();

    if (!clipboard_.get_copied_source()) return;

    // Pastes @p copied at the playhead, keeping the events' spacing, and selects the result.
    //   convert(event)   -> the event to paste, or nullopt to skip it
    //   make_mutator(tx) -> mutator that adds the events
    //   get_selection(tx)-> selection mutator for the pasted kind of event
    const auto paste = [&](const auto* copied, const char* label, auto selection_track,
                           auto&& convert, auto&& make_mutator, auto&& get_selection) {
        if (copied == nullptr || copied->empty()) return;
        using Event = typename std::decay_t<decltype(*copied)>::value_type;

        // the clipboard is not guaranteed to be sorted, so front() is not necessarily the earliest
        const midi::MIDITick first_tick = std::ranges::min_element(*copied, {},
            [](const auto& e) { return e.tick; })->tick;
        const midi::MIDITick playhead_tick = editor_context()->playback->playback_start_tick();

        std::vector<Event> pasted;
        pasted.reserve(copied->size());
        for (const auto& event : *copied) {
            std::optional<Event> converted = convert(event);
            if (!converted) continue;

            converted->tick = converted->tick - first_tick + playhead_tick;
            pasted.push_back(std::move(*converted));
        }
        if (pasted.empty()) return;

        auto tx = _controller->begin_action(label);
        auto mutator = make_mutator(tx);

        using Ticket = decltype(mutator.add_event(std::declval<Event>()));
        std::vector<Ticket> tickets;
        tickets.reserve(pasted.size());
        for (auto& event : pasted) {
            tickets.push_back(mutator.add_event(std::move(event)));
        }

        std::vector<std::size_t> final_ids;
        final_ids.reserve(tickets.size());
        for (const auto& ticket : tickets) {
            final_ids.push_back(mutator.final_id(ticket));
        }

        auto selection = get_selection(tx);
        selection.set(selection_track, final_ids);

        // the selection is now the pasted events; boxes from the old selection are stale
        if (commit_and_bump(tx)) clear_committed_selection_boxes();
    };

    if (src == EventSourceKind::Tempo) {
        paste(clipboard_.get_copied_metas(), "Paste tempo events", history::kGlobalTrack,
            [](const midi::MetaEvent& tempo) -> std::optional<midi::MetaEvent> { return tempo; },
            [](history::Transaction& tx) { return tx.global_meta_events({}); },
            [](history::Transaction& tx) { return tx.selection().meta_events(); });
        return;
    }

    if (!is_channel_event_source(src)) return;

    const auto [src_min, src_max] = EventSources::get_source(clipboard_.get_copied_source()->event_source)->info.range;
    const auto [dst_min, dst_max] = EventSources::get_source(src)->info.range;

    const auto remap_value = [&](double value) -> double {
        if (src_min == dst_min && src_max == dst_max) return value;
        if (src_min == src_max) return dst_min;
        return util::remap_range(value, src_min, src_max, dst_min, dst_max);
    };

    const auto convert_event = [&](const midi::ChannelEvent& event) -> std::optional<midi::ChannelEvent> {
        const std::uint8_t channel = curr_source_.channel == ALL_CHANNELS
            ? event.channel
            : static_cast<std::uint8_t>(curr_source_.channel & 0x0F);
        const bool is_program = std::holds_alternative<midi::ProgramChange>(event.event_type);

        // instrument changes carry no value to remap: they only paste into an instrument lane
        if (src == EventSourceKind::ProgramChange) {
            if (!is_program) return std::nullopt;
            midi::ChannelEvent copy = event;
            copy.channel = channel;
            return copy;
        }
        if (is_program) return std::nullopt;

        const std::optional<double> value = event_value(event);
        if (!value) return std::nullopt;
        return make_channel_event(curr_source_, event.tick, channel, remap_value(*value));
    };

    paste(clipboard_.get_copied_events(), "Paste events", track, convert_event,
        [&](history::Transaction& tx) { return tx.channel_events(track, {}); },
        [](history::Transaction& tx) { return tx.selection().channel_events(); });
}

void DataEditing::delete_events_by_ids(const std::vector<std::size_t>& ids) {
    if (ids.empty()) return;
    const EventSourceKind src = curr_source_.event_source;
    const std::uint16_t track = lane_track();

    if (src == EventSourceKind::Tempo) {
        const auto* tempos = global_metas();
        if (tempos == nullptr) return;
        const bool keeps_tempo = tempo_kept_at_zero(*tempos, ids);

        auto tx = _controller->begin_action("Delete tempo events");
        auto mutator = tx.global_meta_events(ids);
        if (!keeps_tempo) mutator.add_event(DEFAULT_TEMPO);

        mutator.delete_events(ids);
        auto selection = tx.selection().meta_events();
        selection.remap(mutator);

        if (commit_and_bump(tx)) clear_committed_selection_boxes();
    } else if (is_channel_event_source(src)) {
        auto tx = _controller->begin_action("Delete events");
        auto mutator = tx.channel_events(track, ids);
        mutator.delete_events(ids);
        auto selection = tx.selection().channel_events();
        selection.remap(mutator);
        if (commit_and_bump(tx)) clear_committed_selection_boxes();
    }
    // note velocities belong to their notes, so Velocity falls through and nothing is deleted
}

void DataEditing::delete_selected_events() {
    const auto del = [this](const auto&, const std::vector<std::size_t>& ids) { delete_events_by_ids(ids); };
    with_source_selection(*this, _controller, curr_source_.event_source, lane_track(), del, del);
}

DataEditing::SelectionResult DataEditing::select_events_in_between(MIDITick min_tick, MIDITick max_tick) {
    SelectionResult result{};

    const EventSourceKind src = curr_source_.event_source;
    const std::uint16_t track = lane_track();

    const auto select_from = [&](const auto* events, auto tick_of, auto predicate) {
        if (!events) return;
        result.ids = select_ranged(*events, tick_of, min_tick, max_tick, predicate);
    };

    if (src == EventSourceKind::Velocity) {
        select_from(track_notes(track),
            [](const midi::Note& note) { return note.get_start(); },
            [](const auto&) { return true; });
    } else if (src == EventSourceKind::Tempo) {
        select_from(global_metas(),
            [](const midi::MetaEvent& meta) { return meta.tick; },
            is_tempo);
    } else if (is_channel_event_source(src)) {
        select_from(track_events(track),
            [](const midi::ChannelEvent& event) { return event.tick; },
            [&](const midi::ChannelEvent& event) { return event_matches_source(curr_source_, event); });
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

    const DataEditSource& source = curr_source_;
    const auto [min, max] = EventSources::get_source(source.event_source)->info.range;
    const double value_clamped = std::clamp(value, min, max);

    if (source.event_source != EventSourceKind::ProgramChange) {
        if (max - min <= 0.0) return false;

        // "at the playhead" must land exactly on the playhead: the grid snap used by the ramp
        // tools would otherwise drag the event to the nearest grid line
        const double value_normalized = util::remap_range(value_clamped, min, max, 0.0, 1.0);
        return set_events_in_between(tick, value_normalized, tick, value_normalized, true);
    }

    const std::uint16_t track = lane_track();
    const std::vector<midi::ChannelEvent>* events = track_events(track);
    if (events == nullptr) return false;

    const std::vector<std::size_t> matching_ids = select_ranged(*events,
        [](const midi::ChannelEvent& event) { return event.tick; },
        tick, tick,
        [&](const midi::ChannelEvent& event) { return event_matches_source(source, event); }, true);

    std::array<bool, 16> channels_present{};
    for (const std::size_t id : matching_ids) {
        const std::uint8_t channel = (*events)[id].channel;
        if (channel < channels_present.size()) channels_present[channel] = true;
    }

    auto tx = _controller->begin_action("Change instrument");
    auto channel_mut = tx.channel_events(track, matching_ids);

    if (!matching_ids.empty()) {
        channel_mut.set_program(matching_ids,
            static_cast<std::uint8_t>(coerce_to_source(source.event_source, value)));
    }

    // channels that had no instrument change at the playhead get one
    for_each_target_channel(source, [&](std::uint8_t channel) {
        if (channels_present[channel]) return;
        if (auto event = make_channel_event(source, tick, channel, value)) {
            channel_mut.add_event(std::move(*event));
        }
    });

    return commit_and_bump(tx);
}

bool DataEditing::set_note_velocities_ranged(MIDITick min_tick, std::uint8_t min_velocity,
                                             MIDITick max_tick, std::uint8_t max_velocity) {
    const std::uint16_t track = lane_track();

    const std::vector<midi::Note>* notes = track_notes(track);
    if (notes == nullptr) return false;

    std::vector<std::size_t> note_ids{};
    std::vector<std::uint8_t> new_velocities{};

    for (std::size_t i = 0; i < notes->size(); i++) {
        const MIDITick note_tick = (*notes)[i].get_start();
        if (note_tick < min_tick || note_tick > max_tick) continue;

        const double weight = ramp_weight(tool_, ramp_factor(note_tick, min_tick, max_tick));
        const double velocity = std::lerp(static_cast<double>(min_velocity),
                                          static_cast<double>(max_velocity), weight);

        // a velocity of 0 is a note-off in the MIDI format, so the encoded range starts at 1
        note_ids.push_back(i);
        new_velocities.push_back(static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(std::lround(velocity)), 1, 127)));
    }

    if (note_ids.empty()) {
        return false;
    }

    auto tx = _controller->begin_action("Set note velocities");
    auto notes_mutator = tx.notes(track, note_ids);
    notes_mutator.set_velocity(note_ids, new_velocities);

    if (!commit_and_bump(tx)) {
        util::Debugger::log_error("Failed to commit note velocities action");
        return false;
    }
    return true;
}

bool DataEditing::set_events_in_between(MIDITick min_tick, double min_val, MIDITick max_tick, double max_val,
    bool force_no_snap) {
    const DataEditSource& source = curr_source_;
    const EventSourceKind src = source.event_source;
    const auto [min, max] = EventSources::get_source(src)->info.range;
    const double min_mapped = util::remap_range(min_val, 0.0, 1.0, min, max);
    const double max_mapped = util::remap_range(max_val, 0.0, 1.0, min, max);

    const double tick_interval = get_tick_interval();
    const bool snap = !force_no_snap && tick_interval > 1.0;

    // snap min and max ticks to the nearest grid if snapping is enabled and not forced off
    if (snap) {
        snap_tick_to_resolution(min_tick);
        snap_tick_to_resolution(max_tick);
    }

    const RampSpec ramp{ min_tick, max_tick, snap, tick_interval };
    const auto value_at = [&](double x) {
        return std::lerp(min_mapped, max_mapped, ramp_weight(tool_, x));
    };

    if (src == EventSourceKind::Velocity) {
        return set_note_velocities_ranged(min_tick,
            static_cast<std::uint8_t>(coerce_to_source(src, min_mapped)),
            max_tick,
            static_cast<std::uint8_t>(coerce_to_source(src, max_mapped)));
    }

    if (src == EventSourceKind::Tempo) {
        const std::vector<midi::MetaEvent>* metas = global_metas();
        if (metas == nullptr) return false;
        auto tx = _controller->begin_action("Set tempo");

        const auto clamp_bpm = [&](double bpm) { return std::clamp(bpm, min, max); };

        add_and_replace_in_between(*metas, ramp, is_tempo,
            [&](std::vector<std::size_t> ids) { return tx.global_meta_events(ids); },
            // round-trip through microseconds so coerced values are always a representable tempo
            [&](double value) { return util::us_to_bpm(util::bpm_to_us(clamp_bpm(value))); },
            value_at,
            [&](history::MetaEventMutator& mutator, const std::size_t id, const double value) {
                mutator.set_tempo(id, clamp_bpm(value));
            },
            [&](history::MetaEventMutator& mutator, const midi::MIDITick tick, const double value) {
                const double bpm = clamp_bpm(value);
                mutator.add_event(midi::MetaEvent{ .tick = tick,
                                                   .event_type = midi::MetaEventType::Tempo,
                                                   .data = util::bpm_to_bytes(bpm) });
            });
        return commit_and_bump(tx);
    }

    if (src == EventSourceKind::ProgramChange) {
        util::Debugger::log_error("Program (patch/instrument) change does not allow ranged edits");
        return false;
    }

    if (!is_channel_event_source(src)) return false;

    // pitch bend, control change, channel pressure and key pressure all edit channel events
    const std::uint16_t track = lane_track();
    const std::vector<midi::ChannelEvent>* events = track_events(track);
    if (events == nullptr) return false;

    auto tx = _controller->begin_action(set_action_label(src));
    add_and_replace_in_between(*events, ramp,
        [&](const midi::ChannelEvent& event) { return event_matches_source(source, event); },
        [&](std::vector<std::size_t> ids) { return tx.channel_events(track, ids); },
        // the coerced value must be exactly what gets stored: otherwise two ramp steps rounding
        // to the same value compare as different and each becomes its own event
        [&](double value) { return static_cast<double>(coerce_to_source(src, value)); },
        value_at,
        [&](history::ChannelEventMutator& mutator, const std::size_t id, const double value) {
            set_event_value(mutator, source, id, coerce_to_source(src, value));
        },
        [&](history::ChannelEventMutator& mutator, const midi::MIDITick tick, const double value) {
            add_event_on_channels(mutator, source, tick, value);
        });
    return commit_and_bump(tx);
}

#pragma endregion

void DataEditing::snap_tick_to_resolution(midi::MIDITick& tick) const {
    const double tick_interval = get_tick_interval();
    // an interval of 1 or less is basically no snapping, so we can skip the calculation
    if (tick_interval <= 1.0) return;

    // round to the nearest grid line. The product used to be truncated by the cast, so a grid
    // of 7.619 ticks put grid line 3 at tick 22 instead of 23 and drifted further from there
    const double grid_line = std::round(static_cast<double>(tick) / tick_interval);
    tick = static_cast<midi::MIDITick>(std::llround(grid_line * tick_interval));
}

double DataEditing::get_tick_interval() const {
    if (tool_.resolution <= 0) return 1.0;
    const std::uint16_t ppq = _controller->get_project_manager()->get_ppq();
    return static_cast<double>(ppq) / static_cast<double>(tool_.resolution);
}

double DataEditing::value_in_effect_at(midi::MIDITick tick) const {
    if (!is_channel_event_source(curr_source_.event_source)) return get_source_default();

    const std::vector<midi::ChannelEvent>* events = track_events(lane_track());
    if (events == nullptr) return get_source_default();

    // events are tick-sorted; everything before the first event past `tick` is a candidate
    const auto past = std::upper_bound(events->begin(), events->end(), tick,
        [](midi::MIDITick t, const midi::ChannelEvent& e) { return t < e.tick; });

    const auto in_effect = std::find_if(std::make_reverse_iterator(past), events->rend(),
        [&](const midi::ChannelEvent& e) { return event_matches_source(curr_source_, e); });
    if (in_effect == events->rend()) return get_source_default();

    return event_value(*in_effect).value_or(get_source_default());
}

void DataEditing::update_stats() {
    const auto& src = curr_source_.event_source;
    const auto [min, max] = EventSources::get_source(src)->info.range;

    auto& stats = stats_;

    // we have a small margin because mouse pos was clamped already.
    // values would've never exceeded the range,but we want to avoid showing stats when the mouse is outside the rect
    stats.stats_present = mouse_pos_.x >= 0.01f && mouse_pos_.x <= 0.99f && mouse_pos_.y >= 0.01f && mouse_pos_.y <= 0.99f;

    if (!stats.stats_present) return;

    stats.value_at_mouse = util::remap_range(1.0 - static_cast<double>(mouse_pos_.y), 0.0, 1.0, min, max);

    const midi::MIDITick curr_tick = tick_at_norm_x(mouse_pos_.x);

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
        // this used to copy the mouse's own value, so the "current" readout never showed what the
        // lane actually holds at that x position
        stats.present_value_at_mouse = value_in_effect_at(curr_tick);
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
    return { norm_to_screen(work_rect, a.x, a.y), norm_to_screen(work_rect, b.x, b.y) };
}

#pragma region selection overlays

overlays::TransformFn DataEditing::make_overlay_transform() const {
    return [this](const overlays::OverlayRect& rect) -> overlays::ScreenRect {
        // rect's x pos is already in midi ticks, and y pos is already normalized
        const auto& rc = context().rect;
        const auto& nav = navigation()->value;

        const auto tick_to_norm = [&nav](auto tick) {
            return (static_cast<double>(tick) - static_cast<double>(nav.tick_pos_smoothed)) /
                   static_cast<double>(nav.zoom_ticks_smoothed);
        };

        return { norm_to_screen(rc, tick_to_norm(rect.first.x), static_cast<double>(rect.first.y)),
                 norm_to_screen(rc, tick_to_norm(rect.second.x), static_cast<double>(rect.second.y)) };
    };
}

void DataEditing::begin_live_selection_box(midi::MIDITick start, overlays::SelectionType type) {
    if (live_selection_box != nullptr) {
        util::Debugger::log_warning("Live selection box already exists");
        return;
    }
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

double DataEditing::get_source_default() const {
    switch (curr_source_.event_source) {
    case EventSourceKind::PitchBend: return 0;
    case EventSourceKind::Velocity: return 1;
    case EventSourceKind::Tempo: return 120;
    case EventSourceKind::ControlChange:
        switch (curr_source_.controller) {
        case 7:  return 100; // Volume
        case 8:  return 64;  // Balance
        case 10: return 64;  // Pan
        case 11: return 127; // Expression
        case 74: return 64;  // Brightness
        case 91: return 40;  // Reverb Send
        default: return 0;
        }
    default: return 0;
    }
}
#pragma endregion

#pragma endregion

}