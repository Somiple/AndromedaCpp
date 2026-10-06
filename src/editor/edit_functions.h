#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "app/custom_widgets.h"
#include "app/ui/dialog.h"
#include "editor/actions.h"
#include "editor/util.h"
#include "midi/events/note.h"

namespace andromeda::editor {

class NoteEditing;
class EditorController;

namespace edit_fn {

struct FlipX {
    std::vector<std::size_t> note_ids;
};
struct FlipY {
    std::vector<std::size_t> note_ids;
};
struct Stretch {
    std::vector<std::size_t> note_ids;
    float factor;
};
struct Chop {
    std::vector<std::size_t> note_ids;
    MIDITick max_tick_len;
};
struct Glue {
    std::vector<std::size_t> note_ids;
    MIDITick glue_threshold;
    bool separate_channels;
};
struct SetChannel {
    std::uint8_t channel;
};
struct RemoveOverlaps {};
struct SliceAtTick {
    std::vector<std::size_t> note_ids;
    MIDITick slice_tick;
};
struct FadeNotes {
    bool fade_out;
};
struct Transpose {
    SignedMIDIKey amount;
};

}

using EditFunction =
    std::variant<edit_fn::FlipX, edit_fn::FlipY, edit_fn::Stretch, edit_fn::Chop, edit_fn::Glue,
                 edit_fn::SetChannel, edit_fn::RemoveOverlaps, edit_fn::SliceAtTick,
                 edit_fn::FadeNotes, edit_fn::Transpose>;

class EditFunctions {
public:
    void apply_function(std::vector<midi::Note>& notes, std::vector<std::size_t>& sel_note_ids,
                        EditFunction func, std::uint16_t curr_track,
                        EditorActions& editor_actions);
};

class EFDialogBase : public app::Dialog {
public:
    EFDialogBase() = default;
    EFDialogBase(editor::EditorController* controller)
        : _controller(controller) {}

protected:
    void apply(const std::function<EditFunction(const std::vector<std::size_t>&)>& make_func);

    editor::EditorController* _controller;
};

class EFStretchDialog final : public EFDialogBase {
public:
    using EFDialogBase::EFDialogBase;

    app::MaybeDlgAction draw(const app::ImageResources& images) override;
    [[nodiscard]] std::optional<app::DialogActionButtons> get_action_buttons() const override;
    [[nodiscard]] const char* get_dialog_name() const override {
        return app::dialog_names::DIALOG_NAME_EF_STRETCH;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return "Stretch Selection"; }

    app::NumericField<float> stretch_factor{1.0f, 0.0f, std::nullopt};
};

class EFChopDialog final : public EFDialogBase {
public:
    using EFDialogBase::EFDialogBase;

    app::MaybeDlgAction draw(const app::ImageResources& images) override;
    [[nodiscard]] std::optional<app::DialogActionButtons> get_action_buttons() const override;

    // fixed rust bug: returned the stretch name, so chop and stretch closed each other
    [[nodiscard]] const char* get_dialog_name() const override {
        return app::dialog_names::DIALOG_NAME_EF_CHOP;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return "Chop selection"; }

    app::NumericField<MIDITick> target_tick_len{240, 1, std::numeric_limits<MIDITick>::max()};
};

class EFGlueDialog final : public EFDialogBase {
public:
    using EFDialogBase::EFDialogBase;

    app::MaybeDlgAction draw(const app::ImageResources& images) override;
    [[nodiscard]] std::optional<app::DialogActionButtons> get_action_buttons() const override;
    [[nodiscard]] const char* get_dialog_name() const override {
        return app::dialog_names::DIALOG_NAME_EF_GLUE;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return "Glue notes"; }

    app::NumericField<MIDITick> glue_threshold{0, 0, std::numeric_limits<MIDITick>::max()};
    bool separate_channels = true;
};

class EFSetChannelDialog final : public EFDialogBase {
public:
    using EFDialogBase::EFDialogBase;

    app::MaybeDlgAction draw(const app::ImageResources& images) override;
    [[nodiscard]] std::optional<app::DialogActionButtons> get_action_buttons() const override;
    [[nodiscard]] const char* get_dialog_name() const override {
        return app::dialog_names::DIALOG_NAME_EF_SET_CHANNEL;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return "Set channel of notes"; }
    [[nodiscard]] std::uint16_t get_flags() const override {
        return app::dialog_flags::DIALOG_NO_COLLAPSABLE | app::dialog_flags::DIALOG_NO_RESIZABLE;
    }

    app::NumericField<std::uint8_t> new_channel{0, 0, 15};
};

}
