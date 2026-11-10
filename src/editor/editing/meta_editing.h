#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/app/app_event_listener.h"
#include "ui/widgets/custom_widgets.h"
#include "ui/dialogs/dialog.h"
#include "midi/events/meta_event.h"
#include "core/shared.h"
#include "editor/editor_component.h"

namespace andromeda::editor {

class EditorController;

class MetaEditing : public EditorComponent {
public:
    using EditorComponent::EditorComponent;

    void insert_meta_event(midi::MetaEvent meta_event);

    std::vector<midi::MetaEvent> take_metas();
    void set_metas(std::vector<midi::MetaEvent> metas);

    void apply_action(/*EditorAction& action*/);

    // REFACTOR: `has_edited_recently()` and its latch moved into `EditorComponent`,
    // so every editing pass shares the same consume-on-read semantics.
private:
    [[nodiscard]] std::size_t bin_search_metas(MIDITick tick_pos) const;

    std::deque<midi::MetaEvent> tmp_del_metas_;
};

/// Payload for MetaEventInsertDialog::on_open.
/// refactor: the dialog used to be built, configured and pushed through the manager; the
/// registry owns one instance, so the configuration travels in this payload.
struct MetaEventInsertOptions {
    midi::MetaEventType meta_type = midi::MetaEventType::Tempo;              ///< event kind
    std::function<void(std::vector<std::uint8_t>)> on_created{};            ///< receives payload
};

class MetaEventInsertDialog final : public app::Dialog {
public:
    [[nodiscard]] const char* get_title() const override { return title_.c_str(); }
    void draw_content() override;

    /// @param payload must hold a MetaEventInsertOptions; a mismatch is logged.
    void on_open(const app::DialogPayload& payload) override;

    /// Encodes the entered fields and hands them to the payload's callback.
    void on_click_confirm() override;

    [[nodiscard]] const app::DialogActions get_actions() const override {
        return app::DialogActions::OkCancel;
    }

private:
    void init_fields(midi::MetaEventType meta_type);

    std::string title_;
    midi::MetaEventType dialog_type_ = midi::MetaEventType::Tempo;

    std::vector<std::pair<const char*, std::unique_ptr<app::NumberField>>> fields_;

    std::function<void(std::vector<std::uint8_t>)> meta_created_;
};

}
