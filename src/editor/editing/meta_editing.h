#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/app_event_listener.h"
#include "app/custom_widgets.h"
#include "app/ui/dialog.h"
#include "editor/actions.h"
#include "editor/midi_bar_cacher.h"
#include "editor/tempo_map.h"
#include "midi/events/meta_event.h"
#include "util/shared.h"
#include "editor/editor_component.h"

namespace andromeda::editor {

class EditorController;

class MetaEditing : public EditorComponent {
public:
    using EditorComponent::EditorComponent;

    void insert_meta_event(midi::MetaEvent meta_event);

    std::vector<midi::MetaEvent> take_metas();
    void set_metas(std::vector<midi::MetaEvent> metas);

    void apply_action(EditorAction& action);

    // TODO: move this into some base "EditorComponent" class
    bool has_edited_recently() {
        bool last_edited = _edited_recently;
        _edited_recently = false;
        return last_edited;
    }

private:
    [[nodiscard]] std::size_t bin_search_metas(MIDITick tick_pos) const;

    std::deque<midi::MetaEvent> tmp_del_metas_;
    bool _edited_recently = false;
};

class MetaEventInsertDialog final : public app::Dialog {
public:
    app::MaybeDlgAction draw(const app::ImageResources& images) override;
    std::expected<void, std::string> cleanup_dialog() override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return app::dialog_names::DIALOG_NAME_INSERT_META;
    }
    [[nodiscard]] std::string get_dialog_title() const override;
    [[nodiscard]] std::optional<app::DialogActionButtons> get_action_buttons() const override;
    [[nodiscard]] std::uint16_t get_flags() const override {
        return app::dialog_flags::DIALOG_NO_COLLAPSABLE | app::dialog_flags::DIALOG_NO_RESIZABLE;
    }

    void init_meta_dialog(midi::MetaEventType meta_type,
                          std::function<void(std::vector<std::uint8_t>)> on_meta_created);

private:
    bool is_showing_ = false;
    midi::MetaEventType dialog_type_ = midi::MetaEventType::Lyric;

    std::vector<std::pair<const char*, std::unique_ptr<app::NumberField>>> fields_;

    std::function<void(std::vector<std::uint8_t>)> meta_created_;
};

}
