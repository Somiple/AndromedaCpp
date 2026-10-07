#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "app/app_event_listener.h"
#include "app/editor_tool.h"
#include "editor/actions.h"
#include "editor/editing.h"
#include "editor/navigation.h"
#include "editor/util.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"
#include "util/shared.h"
#include "editor/editor_component.h"

namespace andromeda::editor {

class EditorController;
class ProjectManager;

inline constexpr float MIN_DRAGGABLE_WIDTH = 6.0f;
inline constexpr float END_REGION = 4.0f;

namespace note_edit_flags {
inline constexpr std::uint16_t NOTE_EDIT_FLAGS_NONE = 0x0;
inline constexpr std::uint16_t NOTE_EDIT_MOUSE_OVER_UI = 0x1;
inline constexpr std::uint16_t NOTE_EDIT_MOUSE_OVER_NOTE = 0x2;
inline constexpr std::uint16_t NOTE_EDIT_MOUSE_DOWN_ON_UI = 0x4;
inline constexpr std::uint16_t NOTE_EDIT_ANY_DIALOG_OPEN = 0x8;
inline constexpr std::uint16_t NOTE_EDIT_LENGTH_CHANGE = 0x10;
inline constexpr std::uint16_t NOTE_EDIT_DRAGGING = 0x20;
inline constexpr std::uint16_t NOTE_EDIT_MULTIEDIT = 0x40;
inline constexpr std::uint16_t NOTE_EDIT_IS_EDITING = 0x80;
inline constexpr std::uint16_t NOTE_EDIT_ERASING = 0x100;
inline constexpr std::uint16_t NOTE_EDIT_SYNTH_PLAY = 0x200;
inline constexpr std::uint16_t NOTE_EDIT_SHIFT_DOWN = 0x400;
inline constexpr std::uint16_t NOTE_EDIT_MOUSE_OVER_ROLL = 0x800;
}

enum class EditCursor { Default, ResizeHorizontal, Move, Crosshair };

struct NoteEditMouseInfo {
    std::pair<MIDITick, std::uint8_t> mouse_midi_pos{0, 0};
    std::pair<MIDITick, std::uint8_t> mouse_midi_pos_rounded{0, 0};
    std::pair<MIDITick, std::uint8_t> last_mouse_click_pos{0, 0};
    std::optional<std::size_t> last_clicked_note_idx;
    std::pair<MIDITick, std::uint8_t> last_clicked_note_pos{0, 0};
    std::optional<std::size_t> note_hover_idx;
    bool is_at_note_end = false;
};

class EditorController;

class NoteEditing : public EditorComponent {
public:
    using EditorComponent::EditorComponent;

    void update() override;

    void on_mouse_down() override;
    void on_right_mouse_down() override;
    void on_mouse_move() override;
    void on_mouse_up() override;

    struct KeyState {
        bool copy = false;
        bool cut = false;
        bool paste = false;
        bool duplicate = false;
        bool del = false;
    };
    void on_key_down(const KeyState& keys);

    [[nodiscard]] std::pair<MIDITick, std::uint8_t> get_clicked_note_pos() const {
        return mouse_info_.last_clicked_note_pos;
    }

    [[nodiscard]] std::vector<std::pair<MIDITick, std::uint8_t>> get_note_positions(
        const std::vector<std::size_t>& ids) const;
    [[nodiscard]] std::vector<std::pair<std::size_t, MIDITick>> get_note_lengths(
        const std::vector<std::size_t>& ids) const;

    [[nodiscard]] std::vector<midi::Note>* get_ghost_notes() {
        return &ghost_notes_;
    }

    std::vector<midi::Note> take_notes_in_track(std::uint16_t track);
    void set_notes_in_track(std::uint16_t track, std::vector<midi::Note> notes);

    void duplicate_selected_notes();
    std::vector<std::size_t> duplicate_notes(std::uint16_t track,
                                             const std::vector<std::size_t>& ids);
    [[nodiscard]] std::vector<midi::Note> clone_notes(std::uint16_t track, const std::vector<std::size_t>& ids);

    void copy_notes(std::uint16_t track);
    void cut_selected_notes(std::uint16_t track);
    void paste_notes_offset(std::uint16_t track, MIDITick tick_pos);
    void paste_notes(std::uint16_t track);

    void delete_notes(std::vector<std::size_t> ids);
    void delete_notes_no_remap(std::vector<std::size_t> ids);

    void apply_action(EditorAction& action);

    void update_toolbar_settings_from_note(const midi::Note& note) const;
    void update_toolbar_settings_from_clicked_note() const;

    [[nodiscard]] bool get_can_draw_selection_box() const { return draw_select_box_; }

    [[nodiscard]] std::pair<util::math::Vector2<float>, util::math::Vector2<float>>
        get_selection_range_ui() const;

    [[nodiscard]] EditCursor get_cursor() const;

    [[nodiscard]] const NoteEditMouseInfo& mouse_info() const { return mouse_info_; }

    MIDITick latest_note_start = 38400;
    std::tuple<MIDITick, MIDITick, std::uint8_t, std::uint8_t> selection_range{ 0, 0, 0, 0 };

private:
    void update_clicked_note();
    [[nodiscard]] std::optional<std::size_t> get_clicked_note_idx() const {
        return mouse_info_.last_clicked_note_idx;
    }

    void pencil_mouse_down();
    void pencil_mouse_move();
    void pencil_mouse_up();

    void select_mouse_down();
    void select_mouse_move();
    void select_mouse_up();

    void eraser_mouse_down();
    void eraser_mouse_move();
    void eraser_mouse_up();

    void offset_ghost_notes_tmp();
    void offset_notes_length_tmp();
    void init_selection_box(std::pair<MIDITick, std::uint8_t> start_pos);
    void update_selection_box(std::pair<MIDITick, std::uint8_t> new_pos);
    [[nodiscard]] std::tuple<MIDITick, MIDITick, std::uint8_t, std::uint8_t> get_selection_range()
        const;
    void clear_selected();
    void select_notes(std::uint16_t track, std::vector<std::size_t> ids, SelectionOp selection_op);

    void offset_note_lengths(SignedMIDITick length_delta);
    void apply_note_length_change();

    void selected_notes_to_ghost_notes();
    void note_id_as_first_ghost_note(std::size_t id);
    void update_first_ghost_note();
    void set_first_ghost_note_pos(MIDITick start, std::uint8_t key, bool apply_tbs = true);
    void offset_ghost_notes(std::pair<SignedMIDITick, std::int16_t> pos_delta);
    std::vector<midi::Note> ghost_notes_into_notes();
    std::vector<std::size_t> merge_ghost_notes(std::uint16_t track);
    void apply_ghost_place_notes();
    void apply_ghost_move_notes(std::vector<std::pair<SignedMIDITick, std::int16_t>> pos_deltas);
    [[nodiscard]] std::vector<std::pair<SignedMIDITick, std::int16_t>> get_ghost_notes_pos_delta()
        const;

    std::vector<midi::Note> take_notes_curr_track();
    void update_latest_note_start();

    [[nodiscard]] std::tuple<std::uint8_t, MIDITick, std::uint8_t> get_tbs_values() const;
    [[nodiscard]] SignedMIDITick snap_tick(SignedMIDITick tick) const;
    [[nodiscard]] MIDITick get_min_snap_tick_length() const;

    [[nodiscard]] util::math::Vector2<float>
        midi_pos_to_ui_pos(MIDITick tick_pos, std::uint8_t key_pos) const;

    // because c++, we need a getter for the project manager.. lol
    editor::ProjectManager* get_project_manager();

    template <typename F>
    auto with_notes(std::size_t track, F&& f) {
        std::vector<midi::MIDITrack>* tracks = get_project_manager()->get_tracks();
        return f(tracks->at(track).get_notes());
    }

    template <typename F>
    auto with_notes_mut(std::size_t track, F&& f) {
        std::vector<midi::MIDITrack>* tracks = get_project_manager()->get_tracks();
        return f(tracks->at(track).get_notes_mut());
    }

    std::vector<midi::Note> ghost_notes_{};
    NoteEditMouseInfo mouse_info_;

    std::vector<std::pair<MIDITick, std::uint8_t>> note_old_positions_;
    std::vector<std::pair<std::size_t, MIDITick>> note_old_lengths_;

    bool draw_select_box_ = false;
};

}
