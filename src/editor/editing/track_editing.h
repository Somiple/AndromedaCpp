#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "app/app_event_listener.h"
#include "app/editor_tool.h"
#include "app/view_settings.h"
#include "editor/actions.h"
#include "editor/editing.h"
#include "editor/navigation.h"
#include "editor/playhead.h"
#include "editor/project/project_manager.h"
#include "editor/util.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"
#include "util/shared.h"
#include "editor/editor_component.h"

namespace andromeda::editor {
   
class EditorController;

namespace track_flags {
inline constexpr std::uint16_t TRACK_EDIT_FLAGS_NONE = 0x0;
inline constexpr std::uint16_t TRACK_EDIT_MOUSE_OVER_UI = 0x1;
inline constexpr std::uint16_t TRACK_EDIT_MOUSE_DOWN_ON_UI = 0x2;
inline constexpr std::uint16_t TRACK_EDIT_SELECTION_MOVE = 0x4;
inline constexpr std::uint16_t TRACK_EDIT_ANY_DIALOG_OPEN = 0x8;
inline constexpr std::uint16_t TRACK_EDIT_ERASING = 0x10;

inline constexpr std::uint16_t TRACK_EDIT_SHIFT_DOWN = 0x100;
}

struct TrackEditMouseInfo {
    std::pair<float, float> mouse_pos{0.0f, 0.0f};
    std::pair<MIDITick, std::uint16_t> mouse_midi_track_pos{0, 0};
    std::pair<MIDITick, std::uint16_t> last_mouse_click_pos{0, 0};
};

using GhostTrackNotes = std::vector<std::pair<std::uint16_t, std::vector<midi::Note>>>;

class TrackEditing : public EditorComponent {
public:
    using EditorComponent::EditorComponent;

    void update() override;

    void on_mouse_down() override;
    void on_right_mouse_down() override;
    void on_mouse_move() override;
    void on_mouse_up() override;

    struct KeyState {
        bool track_up = false;
        bool track_down = false;
        bool del = false;
        bool copy = false;
        bool cut = false;
        bool paste = false;
    };
    void on_key_down(const KeyState& keys);

    [[nodiscard]] bool get_can_draw_selection_box() const { return draw_select_box_; }

    [[nodiscard]] std::pair<util::math::Vector2<float>, util::math::Vector2<float>> get_selection_range_ui() const;

    [[nodiscard]] bool is_mouse_over_select_area() const;

    [[nodiscard]] util::SharedMutPtr<GhostTrackNotes> get_ghost_notes() const {
        return ghost_notes_;
    }
    [[nodiscard]] util::SharedPtr<SignedMIDITrkVec> get_ghost_note_offset() const {
        return ghost_notes_render_offset_;
    }

    void insert_track(std::uint16_t track);
    void remove_track(std::uint16_t track);
    midi::MIDITrack remove_track_at(std::uint16_t track);

    void decompose_track(std::uint16_t track, bool should_register);

    void append_empty_track();
    void pop_track();

    [[nodiscard]] std::uint16_t get_right_clicked_track() const { return right_clicked_track_; }
    void remove_right_clicked_track();

    [[nodiscard]] std::uint16_t get_used_track_count() const;

    void change_track(std::uint16_t new_track);
    void swap_tracks(std::uint16_t track_1, std::uint16_t track_2);

    void copy_notes();
    void cut_notes();
    void paste_notes(std::uint16_t base_track);

    void apply_action(EditorAction& action);

    std::pair<MIDITick, std::uint16_t> get_mouse_pos() const {
        return mouse_info_.mouse_midi_track_pos;
    }

    [[nodiscard]] MIDITick get_mouse_tick_pos() const {
        return get_mouse_pos().first;
    }

    [[nodiscard]] MIDITick get_mouse_tick_pos_snapped() const {
        return static_cast<MIDITick>(snap_tick(get_mouse_tick_pos()));
    }

    [[nodiscard]] std::uint16_t get_mouse_track_pos() const {
        return get_mouse_pos().second;
    }

    std::tuple<MIDITick, MIDITick, std::uint16_t, std::uint16_t> selection_range{0, 0, 0, 0};
    bool has_selection = false;

private:
    [[nodiscard]] std::pair<MIDITick, std::uint16_t> screen_pos_to_midi_track_pos(util::math::Vector2<float> screen_pos) const;
    [[nodiscard]] util::math::Vector2<float> midi_track_pos_to_screen_pos(std::pair<MIDITick, std::uint16_t> midi_track_pos) const;

    void select_mouse_down();
    void select_mouse_move();
    void select_mouse_up();

    void eraser_mouse_down();
    void eraser_mouse_move();
    void eraser_mouse_up();

    void init_selection_box(std::pair<MIDITick, std::uint16_t> start_pos);
    void update_selection_box(std::pair<MIDITick, std::uint16_t> new_pos);
    [[nodiscard]] std::tuple<MIDITick, MIDITick, std::uint16_t, std::uint16_t> get_selection_range()
        const;

    [[nodiscard]] std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>>
    get_note_ids_in_region(
        const std::tuple<MIDITick, MIDITick, std::uint16_t, std::uint16_t>& region) const;

    void deselect_all();
    void delete_selection();

    void offset_render_ghost_notes(SignedMIDITrkVec offset) const;
    void reset_ghost_note_offset() const;
    void move_ghost_notes(SignedMIDITick tick_change, std::int16_t track_change);
    void selected_notes_to_ghost_notes();
    std::vector<std::pair<std::uint16_t, std::vector<std::size_t>>> apply_ghost_notes();

    [[nodiscard]] bool track_exists(std::uint16_t track) const;

    std::optional<std::vector<midi::Note>> take_notes_in_track(std::uint16_t track);
    std::optional<std::pair<std::vector<midi::Note>, std::vector<midi::Note>>>
    take_some_notes_in_track(std::uint16_t track, const std::vector<std::size_t>& ids);
    void set_notes_in_track(std::uint16_t track, std::vector<midi::Note> notes);

    void insert_notes_and_ch_evs(std::uint16_t track, std::vector<midi::Note> notes,
                                 std::vector<midi::ChannelEvent> ch_evs);
    void insert_track_at(std::uint16_t track_idx, midi::MIDITrack track);

    void swap_tracks_and_register(std::uint16_t track_1, std::uint16_t track_2,
                                  bool allow_register);

    [[nodiscard]] std::vector<midi::Note> clone_notes(std::uint16_t track,
                                                      const std::vector<std::size_t>& ids) const;
    void prepare_clipboard();

    [[nodiscard]] std::pair<MIDITick, std::uint16_t> get_mouse_midi_pos_snapped() const;
    [[nodiscard]] SignedMIDITick snap_tick(SignedMIDITick tick) const;
    [[nodiscard]] MIDITick get_min_snap_tick_length() const;

    TrackEditMouseInfo mouse_info_;

    std::uint16_t right_clicked_track_ = 0;
    MIDITick _playhead_tick = 0;

    bool draw_select_box_ = false;

    util::SharedMutPtr<GhostTrackNotes> ghost_notes_ = util::make_shared_mut<GhostTrackNotes>();
    util::SharedPtr<SignedMIDITrkVec> ghost_notes_render_offset_ =
        util::make_shared_rw<SignedMIDITrkVec>();
};

}
