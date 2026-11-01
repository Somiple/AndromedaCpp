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

namespace andromeda::editor {

class EditorController;

namespace data_edit_flags {
inline constexpr std::uint16_t DATA_EDIT_FLAGS_NONE = 0x0;
inline constexpr std::uint16_t DATA_EDIT_MOUSE_OVER_UI = 0x1;
inline constexpr std::uint16_t DATA_EDIT_MOUSE_DOWN_ON_UI = 0x2;
inline constexpr std::uint16_t DATA_EDIT_ANY_DIALOG_OPEN = 0x4;
inline constexpr std::uint16_t DATA_EDIT_DRAW_EDIT_LINE = 0x8;
inline constexpr std::uint16_t DATA_EDIT_CLICKED_IN_RECT = 0x10;
}

using DataNumType = std::int16_t;

struct DataEditMouseInfo {
    std::pair<MIDITick, DataNumType> mouse_data_pos{0, 0};
    util::math::Vector2<float> mouse_screen_pos{0.0f, 0.0f};
    std::pair<MIDITick, DataNumType> last_data_click_pos{0, 0};
    util::math::Vector2<float> last_screen_click_pos{0.0f, 0.0f};
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
    DataEditToolKind tool_kind = DataEditToolKind::Pencil;
    DataEditCurveKind curve_kind = DataEditCurveKind::Linear;
    float influence = 2.0f; // can range from 1.0 to 5.0
};

class DataEditing : public EditorComponent {
public:
    using EditorComponent::EditorComponent;

    void on_mouse_down() override;
    void on_mouse_move() override;
    void on_mouse_up() override;

    void update() override;

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
    [[nodiscard]] static float curve_weight(DataEditCurveKind kind, float t, float curve_exponent = 2.0f) noexcept;

    /// Writes @p value at the editor's current data position, through the active source's own
    /// edit path, so the widget never has to know what "set" means for a given source.
    /// @param value value in the source's own units; clamped by the source.
    /// @return true when at least one event changed and the action committed.
    bool set_value_at_current(double value);

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
    void select_mouse_down() {}
    void select_mouse_move() {}
    void select_mouse_up() {}

    void set_note_velocities_ranged(MIDITick min_tick, std::uint8_t min_velocity,
                                    MIDITick max_tick, std::uint8_t max_velocity);

    void update_last_mouse_data_pos();

    app::VS_PianoRoll_DataViewState data_view_state;

    DataEditMouseInfo mouse_info_{};
    DataEditSource curr_source_{};
    DataEditTool tool_{};
};

}
