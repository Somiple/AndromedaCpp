#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "ui/app/editor_tool.h"
#include "ui/app/view_settings.h"
#include "editor/navigation.h"
#include "editor/util.h"
#include "midi/midi_track.h"
#include "core/shared.h"
#include "editor/editor_component.h"
#include "editor/editing/data_editing/event_source.h"
#include "editor/overlays/selection_box.h"
#include "editor/editing/selection_gesture.h"

namespace andromeda::editor {

class EditorController;

namespace data_edit_flags {
inline constexpr std::uint16_t DATA_EDIT_FLAGS_NONE = 0x0;
inline constexpr std::uint16_t DATA_EDIT_MOUSE_OVER_UI = 0x1;
inline constexpr std::uint16_t DATA_EDIT_MOUSE_DOWN_ON_UI = 0x2;
inline constexpr std::uint16_t DATA_EDIT_ANY_DIALOG_OPEN = 0x4;
inline constexpr std::uint16_t DATA_EDIT_DRAW_EDIT_LINE = 0x8;
inline constexpr std::uint16_t DATA_EDIT_CLICKED_IN_RECT = 0x10;
inline constexpr std::uint16_t DATA_EDIT_MOUSE_DOWN_IN_SEL = 0x20;
}

using DataNumType = std::int16_t;

struct DataEditMouseInfo {
    /*std::pair<MIDITick, DataNumType> mouse_data_pos{0, 0};
    util::math::Vector2<float> mouse_screen_pos{0.0f, 0.0f};
    std::pair<MIDITick, DataNumType> last_data_click_pos{0, 0};
    util::math::Vector2<float> last_screen_click_pos{0.0f, 0.0f};*/
    util::math::Vector2<float> mouse_anchor{ 0.0f, 0.0f };
};

struct DataEditSource {
    EventSourceKind event_source = EventSourceKind::Velocity;
    std::uint8_t channel = 0xFF; // 0xFF = All channels
    std::uint8_t controller = 0;
    std::uint8_t patch = 0; // 0 = Piano
};

enum DataEditToolKind {
    Pencil,
    Line,
    Curve,
    Selector,
    Eraser
};

enum DataEditCurveKind {
    Linear,
    Exponential,
    Logarithmic,
    SCurve,
    Pulse
};

struct DataEditTool {
    // Line is the default because the Pencil and Eraser tools are not implemented yet, and a
    // default tool that silently does nothing is a bad first impression.
    DataEditToolKind tool_kind = DataEditToolKind::Line;
    DataEditCurveKind curve_kind = DataEditCurveKind::Linear;
    float influence = 2.0f; // can range from 1.0 to 5.0
    // Grid divisions per quarter note: the snap interval is PPQ / resolution ticks.
    // 0 = no snapping, 1 = quarter note, 2 = 1/8, 4 = 1/16, 16 = 1/64, 3 = 1/8 triplet, etc.
    editor::MIDITick resolution = 16;
};

// stats for UI and stuff
struct DataEditStats {
    bool stats_present = false;
    double present_value_at_mouse = 0.0; // reports the value that exists at mouse's x pos.
    double value_at_mouse = 0.0; // reports what data value the mouse's y pos is at
};

class DataEditClipboard {
public:
    void events_to_clipboard(std::vector<midi::ChannelEvent> events, DataEditSource source);
    void events_to_clipboard(std::vector<midi::MetaEvent> metas, DataEditSource source);

    [[nodiscard]] std::vector<midi::MetaEvent>* get_copied_metas();
    [[nodiscard]] std::vector<midi::ChannelEvent>* get_copied_events();

    [[nodiscard]] DataEditSource* get_copied_source();
private:
    using NonNoteEvents = std::variant<
        std::vector<midi::ChannelEvent>, std::vector<midi::MetaEvent>>;

    std::optional<DataEditSource> copied_source_ = std::nullopt;
    NonNoteEvents copied_events_;
};

class DataEditing : public EditorComponent {
public:
    using EditorComponent::EditorComponent;

    void update() override;
    void cleanup() override;

    void on_mouse_down() override;
    void on_mouse_move() override;
    void on_mouse_up() override;

    struct KeyState {
        bool copy = false;
        bool cut = false;
        bool paste = false;
        bool del = false;
    };
    void on_key_down(const KeyState& keys);

    /// Aborts any gesture in progress (edit line, live selection box, selection drag) without
    /// committing anything. Safe to call at any time.
    void cancel_gesture();

    [[nodiscard]] std::pair<util::math::Vector2<float>, util::math::Vector2<float>> get_data_view_line_points() const;

    [[nodiscard]] DataEditSource* get_edit_source() { return &curr_source_; }

    /// @return index of the track this editor is currently drawing; taken from the controller so
    ///         the lane and the editing pass can never disagree about which track is active.
    [[nodiscard]] std::uint16_t lane_track() const;

    /// @param track track to read.
    /// @return the notes of @p track, or nullptr when the track or the project is unavailable.
    [[nodiscard]] const std::vector<midi::Note>* track_notes(std::uint16_t track) const;
    [[nodiscard]] const std::vector<midi::ChannelEvent>* track_events(std::uint16_t track) const;
    [[nodiscard]] const std::vector<midi::MetaEvent>* global_metas() const;

    /// @return the notes of the track currently being drawn, or nullptr when unavailable.
    [[nodiscard]] const std::vector<midi::Note>* lane_notes() const {
        return track_notes(lane_track());
    }

    /// @return the tool selected for this editor: kind plus curve shape. The widget writes the
    ///         selection here so the editing pass and the header can never disagree.
    [[nodiscard]] DataEditTool* get_edit_tool() { return &tool_; }

    /// @param kind curve shape to evaluate.
    /// @param t position along the ramp, 0..1; clamped into range.
    /// @return weight in 0..1 used to mix the two drag endpoints.
    [[nodiscard]] static double curve_weight(DataEditCurveKind kind, double t, double curve_exponent = 2.0f) noexcept;

    /// Writes @p value at the editor's current data position, through the active source's own
    /// edit path, so the widget never has to know what "set" means for a given source.
    /// @param value value in the source's own units; clamped by the source.
    /// @return true when at least one event changed and the action committed.
    bool set_value_at_current(double value);

    [[nodiscard]] const DataEditStats& get_stats() const { return stats_; }

    void delete_selected_events();
    // @returns The default value of the current source.
    double get_source_default() const;
private:
    [[nodiscard]] std::pair<MIDITick, DataNumType> screen_pos_to_data_pos(util::math::Vector2<float> screen_pos) const;
    [[nodiscard]] util::math::Vector2<float> data_pos_to_screen_pos(std::pair<MIDITick, DataNumType> data_pos) const;

    [[nodiscard]] DataNumType scaled_y_from_curr_data(float y) const;
    [[nodiscard]] float unscaled_y_from_curr_data(DataNumType y) const;

    void pencil_mouse_down();
    void pencil_mouse_move();
    void pencil_mouse_up();

    void eraser_mouse_down() {}
    void eraser_mouse_move() {}
    void eraser_mouse_up() {}
    void select_mouse_down();
    void select_mouse_move();
    void select_mouse_up();

    void copy_selected_events();
    void paste_selected_events();

    // selection functions
    struct SelectionResult {
        bool have = false;
        std::vector<std::size_t> ids{};
    };

    // Selects events using the current source.
    // May have to supply a "source" override.
    SelectionResult select_events_in_between(MIDITick min_tick, MIDITick max_tick,
        std::optional<EventSourceKind> source_override = std::nullopt);

    /// @return true when the action committed.
    bool set_note_velocities_ranged(MIDITick min_tick, std::uint8_t min_velocity,
                                    MIDITick max_tick, std::uint8_t max_velocity);
    // min_val and max_val are expected to be in the 0-1 range.
    // @return true when the action committed.
    bool set_events_in_between(MIDITick min_tick, double min_val, MIDITick max_tick, double max_val, bool force_no_snap = false);

    /// @param x_norm horizontal position normalized to the work rect (0..1).
    /// @return the tick under that position, never negative even when the view is scrolled
    ///         before the start of the project.
    [[nodiscard]] midi::MIDITick tick_at_norm_x(double x_norm) const;
    /// @return the tick under the mouse, snapped to the timeline's grid.
    [[nodiscard]] midi::MIDITick snapped_mouse_tick() const;
    /// @return the value of the last event of the current source at or before @p tick, or the
    ///         source's default when there is none.
    [[nodiscard]] double value_in_effect_at(midi::MIDITick tick) const;
    void snap_tick_to_resolution(MIDITick& tick) const;
	// returns how many ticks occupy one interval of the current resolution setting. 1 means no snapping.
	// returns a double because the resolution can be a fraction of a tick, e.g. 1/63 resolution on a 480 PPQ project is 7.619 ticks per interval.
    [[nodiscard]] double get_tick_interval() const;
    void update_last_mouse_data_pos();
    void update_stats();

    [[nodiscard]] const bool source_is_mutable() const {
        if (curr_source_.event_source == EventSourceKind::Velocity) return false;
        return true;
    }

    util::math::Vector2<float> mouse_over_margin() const override {
		return { 20.0f, 0.0f };
    }

    app::VS_PianoRoll_DataViewState data_view_state{};

    util::math::Vector2<float> mouse_anchor_{ 0.0f, 0.0f };
    util::math::Vector2<float> mouse_pos_{ 0.0f, 0.0f };
    
    DataEditSource curr_source_{};
    DataEditTool tool_{};
    DataEditStats stats_{};
    DataEditClipboard clipboard_{};

#pragma region selection overlays
    static constexpr std::uint8_t OVERLAY_LAYER_COMMITTED = 0;   // drawn first
    static constexpr std::uint8_t OVERLAY_LAYER_LIVE = 1;        // drawn on top

    overlays::SelectionBox* live_selection_box = nullptr;
    std::vector<overlays::SelectionBox*> committed_selection_boxes;
    overlays::TransformFn make_overlay_transform() const override;

    void begin_live_selection_box(midi::MIDITick start, overlays::SelectionType type);
	void update_live_selection_box(midi::MIDITick new_pos);
	void end_live_selection_box();
	void add_committed_selection_box(std::pair<midi::MIDITick, midi::MIDITick> range);
	void clear_committed_selection_boxes();
    void offset_committed_selection_boxes(SignedMIDITick tick);
    void translate_committed_selection_boxes(SignedMIDITick tick);

    SelectionGesture m_active_gesture = SelectionGesture::Replace;
    midi::MIDITick subtract_anchor_ = 0;

    midi::MIDITick mouse_tick_anchor_ = 0;
#pragma endregion
};

}