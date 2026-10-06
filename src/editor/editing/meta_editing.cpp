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
#include "util/debugger.h"

#include "editor/editor_controller.h"
#include "app/main_window.h"

namespace andromeda::editor {

using midi::MetaEvent;
using midi::MetaEventType;
using util::Debugger;

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
    const MetaEventType meta_ev_type = meta_event.event_type;

    {
        EditorActions* actions = _controller->get_actions();
        auto& metas = *_controller->get_project_manager()->get_metas();

        const auto it = std::lower_bound(
            metas.begin(), metas.end(), tick,
            [](const MetaEvent& meta, MIDITick t) { return meta.tick < t; });
        const auto insert_idx = static_cast<std::size_t>(it - metas.begin());

        const bool replace_meta =
            insert_idx < metas.size() ? (meta_event.tick == metas[insert_idx].tick &&
                                         meta_event.event_type == metas[insert_idx].event_type)
                                      : false;

        if (replace_meta) {
            MetaEvent old_meta = std::exchange(metas[insert_idx], std::move(meta_event));

            if (actions) {
                std::vector<EditorAction> bulk;
                bulk.push_back(DeleteMeta{{insert_idx}, std::vector<MetaEvent>{old_meta}});
                bulk.push_back(AddMeta{{insert_idx}, std::nullopt});
                actions->register_action(Bulk{std::move(bulk)});
            }

            Debugger::log("Meta event replaced");
        } else {
            metas.insert(metas.begin() + static_cast<std::ptrdiff_t>(insert_idx),
                         std::move(meta_event));

            if (actions) {
                actions->register_action(AddMeta{{insert_idx}, std::nullopt});
            }
        }
    }

    auto* project_manager = _controller->get_project_manager();
    TempoMap* tempo_map = project_manager->get_tempo_map();

    if (meta_ev_type == MetaEventType::Tempo && tempo_map) {
        tempo_map->rebuild_tempo_map(project_manager->get_ppq());
    }

    _edited_recently = true;
}

std::vector<MetaEvent> MetaEditing::take_metas() {
    auto& global_metas = *_controller->get_project_manager()->get_metas();
    return std::exchange(global_metas, {});
}

void MetaEditing::set_metas(std::vector<MetaEvent> metas) {
    auto& global_metas = *_controller->get_project_manager()->get_metas();
    global_metas = std::move(metas);
}

void MetaEditing::apply_action(EditorAction& action) {
    auto* project_manager = _controller->get_project_manager();
    TempoMap* tempo_map = project_manager->get_tempo_map();
    uint16_t ppq = project_manager->get_ppq();

    if (auto* add = std::get_if<AddMeta>(&action.node)) {
        // fixed rust bug: asserted here; a broken undo entry now logs instead
        if (!add->metas) {
            Debugger::log_error(
                "[ADD METAS] Something has gone wrong while undoing meta deletion.");
            return;
        }

        {
            std::vector<MetaEvent> recovered_metas = std::move(*add->metas);
            add->metas.reset();
            std::vector<MetaEvent> old_metas = take_metas();

            set_metas(merge_metas(std::move(old_metas), std::move(recovered_metas)));
        }

        if (tempo_map) {
            tempo_map->rebuild_tempo_map(ppq);
        }

        _edited_recently = true;
    } else if (auto* del = std::get_if<DeleteMeta>(&action.node)) {
        {
            std::vector<MetaEvent> old_metas = take_metas();

            auto [deleted, new_metas] = note_seq::extract(std::move(old_metas), del->meta_ids);
            set_metas(std::move(new_metas));

            del->metas = std::move(deleted);
        }

        if (tempo_map) {
            tempo_map->rebuild_tempo_map(ppq);
        }

        _edited_recently = true;
    }
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
