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
    bump_revision();
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

void MetaEventInsertDialog::on_open(const app::DialogPayload& payload) {
    const auto* options = payload.get_if<MetaEventInsertOptions>();
    if (options == nullptr) {
        util::Debugger::log_error(
            "MetaEventInsertDialog opened without a MetaEventInsertOptions payload");
        return;
    }

    meta_created_ = options->on_created;
    init_fields(options->meta_type);
}

void MetaEventInsertDialog::draw_content() {
    for (auto& [label, field] : fields_) {
        field->show(label, 0.0f);
    }
}

// refactor: the confirm work used to live in the DlgOkCancel "ok" callback; the base now routes
// a confirm into this hook, so the encoding happens in one place.
void MetaEventInsertDialog::on_click_confirm() {
    std::vector<std::uint8_t> data;

    switch (dialog_type_) {
    case MetaEventType::TimeSignature:
        if (fields_.size() >= 2) {
            data = {fields_[0].second->as_u8(), fields_[1].second->as_u8()};
        }
        break;
    case MetaEventType::Tempo:
        if (!fields_.empty()) {
            const auto bytes = tempo_as_bytes(fields_[0].second->as_f32());
            data.assign(bytes.begin(), bytes.end());
        }
        break;
    default:
        break;
    }

    if (!data.empty() && meta_created_) {
        auto callback = std::move(meta_created_);
        meta_created_ = nullptr;
        callback(std::move(data));
    }
}

void MetaEventInsertDialog::init_fields(MetaEventType meta_type) {
    dialog_type_ = meta_type;
    fields_.clear();
    title_ = std::format("Insert {}", midi::to_string(meta_type));

    switch (meta_type) {
    case MetaEventType::TimeSignature:
        fields_.emplace_back("Numerator",
                             std::make_unique<app::NumericField<std::uint8_t>>(4, 1, 12));
        fields_.emplace_back("Denominator (Power of 2)",
                             std::make_unique<app::NumericField<std::uint8_t>>(2, 0, 4));
        break;
    case MetaEventType::Tempo:
        fields_.emplace_back("Tempo", std::make_unique<app::NumericField<float>>(
                                           120.0f, 60000000.0f / static_cast<float>(0xFFFFFF),
                                           60000000.0f / 1.0f));
        break;
    default:
        break;
    }
}

}
