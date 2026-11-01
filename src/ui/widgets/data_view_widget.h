#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ui/rendering/note_gpu_cache.h"
#include "ui/widgets/editor_widget.h"
#include "editor/editing/data_editing.h"
#include "editor/editor_component.h"

namespace andromeda::editor {

class EventLane;

}

namespace andromeda::app {

namespace rendering {
class DataViewRenderer;
}

// the strip under the piano roll; its dock panel lays it out and queues its gl pass
class DataViewWidget final : public EditorWidget {
public:
    // must run after glad has loaded the gl functions; draws out of the piano roll notes
    DataViewWidget(MainWindow& parent, std::shared_ptr<rendering::NoteGpuCache> note_cache);

    /// Out of line so the unique_ptr<EventLane> member can be destroyed where EventLane is
    /// complete; an inline destructor would need the definition in every translation unit.
    ~DataViewWidget() override;

    [[nodiscard]] std::shared_ptr<rendering::DataViewRenderer> renderer() const;

    /// The gl pass is scissored to the lane body only, so the header row keeps the panel
    /// background and the renderer can never paint over it.
    [[nodiscard]] editor::ViewRect gl_clip_rect() const override;
    [[nodiscard]] editor::EditorComponent* get_component() const override {
        return data_editing_;
    }

    void draw_ui() override;
protected:
    void handle_input() override;
    void draw_overlays() override;

private:
    /// Rebuilds the editable lane and its onion ghosts when any projection input changed.
    void refresh_lane();

    /// @param active_track track currently being edited.
    /// @return onion tracks selected by the view settings; never includes @p active_track.
    [[nodiscard]] std::vector<std::uint16_t> onion_tracks(std::uint16_t active_track) const;

    /// @return key covering every input the projection depends on; 0 means "rebuild always".
    [[nodiscard]] std::uint64_t lane_key() const;

    editor::DataEditing* data_editing_ = nullptr;
    std::unique_ptr<editor::EventLane> event_lane_;
    std::uint64_t lane_key_ = 0;
};

}
