#pragma once

#include <memory>

#include "app/rendering/note_gpu_cache.h"
#include "app/widgets/editor_widget.h"
#include "editor/editing/data_editing.h"

namespace andromeda::app {

namespace rendering {
class DataViewRenderer;
}

// the strip under the piano roll; its dock panel lays it out and queues its gl pass
class DataViewWidget final : public EditorWidget {
public:
    // must run after glad has loaded the gl functions; draws out of the piano roll notes
    DataViewWidget(MainWindow& parent, std::shared_ptr<rendering::NoteGpuCache> note_cache);

    [[nodiscard]] std::shared_ptr<rendering::DataViewRenderer> renderer() const;

protected:
    void handle_input() override;
    void draw_overlays() override;

private:
    editor::DataEditing* data_editing_ = nullptr;
};

}
