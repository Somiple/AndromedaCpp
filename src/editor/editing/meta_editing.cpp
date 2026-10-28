#include "editor/editing/meta_editing.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include <imgui.h>

#include "editor/editing/meta_editing/meta_sequence_funcs.h"
#include "editor/editing/note_editing/note_sequence_funcs.h"
#include "editor/util.h"
#include "core/debugger.h"

#include "editor/editor_controller.h"
#include "ui/app/main_window.h"

namespace andromeda::editor {

using midi::MetaEvent;
using midi::MetaEventType;
using util::Debugger;

using history::EventId;

std::size_t MetaEditing::bin_search_metas(MIDITick tick_pos) const {
    const auto& metas = *_controller->get_project_manager()->get_metas();
    if (metas.empty()) {
        return 0;
    }

    std::size_t low = 0;
    std::size_t high = metas.size();

    while (low < high) {
        const std::size_t mid = (low + high) / 2;
        if (metas[mid].tick <= tick_pos) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    return low;
}

void MetaEditing::insert_meta_event(MetaEvent meta_event) {
    const MIDITick tick = meta_event.tick;
    const MetaEventType type = meta_event.event_type;

    const auto& metas = _controller->get_project_manager()->get_project_data().global_metas;

    // same tick AND same type -> replace. Several events can share a tick, so scan the whole run.
    // (SequencerSpecific/Text/Lyric/Marker are legitimately repeatable at one tick; see below.)
    std::optional<EventId> replace_idx;
    auto it = std::lower_bound(
        metas.begin(), metas.end(), tick,
        [](const MetaEvent& m, MIDITick t) { return m.tick < t; });
    for (; it != metas.end() && it->tick == tick; ++it) {
        if (it->event_type == type) {
            replace_idx = static_cast<EventId>(it - metas.begin());
            break;
        }
    }

    auto tx = _controller->begin_action(replace_idx ? "Replace meta event" : "Add meta event");

    std::array<EventId, 1> declared{ replace_idx.value_or(0) };
    auto meta_mutator = tx.global_meta_events(
        replace_idx ? std::span<const EventId>(declared) : std::span<const EventId>{});

    if (replace_idx) {
        meta_mutator.delete_event(*replace_idx);
        Debugger::log("Meta event replaced");
    }
    meta_mutator.add_event(meta_event);

    if (!tx.commit()) {
        return;   // rejected + rolled back; don't flag as edited
    }
    mark_edited();
}

std::vector<MetaEvent> MetaEditing::take_metas() {
    auto& global_metas = *_controller->get_project_manager()->get_metas();
    return std::exchange(global_metas, {});
}

void MetaEditing::set_metas(std::vector<MetaEvent> metas) {
    auto& global_metas = *_controller->get_project_manager()->get_metas();
    global_metas = std::move(metas);
}

void MetaEditing::apply_action(/*EditorAction& action*/) {
    Debugger::log_error("DEPRECATED: use transaction api for undo/redo actions instead");
}

app::MaybeDlgAction MetaEventInsertDialog::draw(const app::ImageResources&) {
    for (auto& [label, field] : fields_) {
        field->show(label, 0.0f);
    }
    return std::nullopt;
}

std::expected<void, std::string> MetaEventInsertDialog::cleanup_dialog() {
    fields_.clear();
    return {};
}

std::string MetaEventInsertDialog::get_dialog_title() const {
    return std::format("Insert {}", midi::to_string(dialog_type_));
}

std::optional<app::DialogActionButtons> MetaEventInsertDialog::get_action_buttons() const {
    return app::DlgOkCancel{
        [](app::Dialog& dlg) -> app::MaybeDlgAction {
            auto& self = static_cast<MetaEventInsertDialog&>(dlg);

            std::vector<std::uint8_t> data;

            switch (self.dialog_type_) {
            case MetaEventType::TimeSignature:
                if (self.fields_.size() >= 2) {
                    data = {self.fields_[0].second->as_u8(), self.fields_[1].second->as_u8()};
                }
                break;
            case MetaEventType::Tempo:
                if (!self.fields_.empty()) {
                    const auto bytes = tempo_as_bytes(self.fields_[0].second->as_f32());
                    data.assign(bytes.begin(), bytes.end());
                }
                break;
            default:
                break;
            }

            if (!data.empty() && self.meta_created_) {
                auto callback = std::move(self.meta_created_);
                self.meta_created_ = nullptr;
                callback(std::move(data));
            }

            return app::DialogClose{self.get_dialog_name()};
        },
        app::dialog_default_close_action()};
}

void MetaEventInsertDialog::init_meta_dialog(
    MetaEventType meta_type, std::function<void(std::vector<std::uint8_t>)> on_meta_created) {
    dialog_type_ = meta_type;

    switch (meta_type) {
    case MetaEventType::TimeSignature:
        fields_.clear();
        fields_.emplace_back("Numerator",
                             std::make_unique<app::NumericField<std::uint8_t>>(4, 1, 12));
        fields_.emplace_back("Denominator (Power of 2)",
                             std::make_unique<app::NumericField<std::uint8_t>>(2, 0, 4));
        meta_created_ = std::move(on_meta_created);
        is_showing_ = true;
        break;
    case MetaEventType::Tempo:
        fields_.clear();
        fields_.emplace_back("Tempo", std::make_unique<app::NumericField<float>>(
                                          120.0f, 60000000.0f / static_cast<float>(0xFFFFFF),
                                          60000000.0f / 1.0f));
        meta_created_ = std::move(on_meta_created);
        is_showing_ = true;
        break;
    default:
        break;
    }
}

}
