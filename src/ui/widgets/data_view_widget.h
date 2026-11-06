#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <optional>

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

    void draw_lane_overlays();

#pragma region Header row
    /// The header is one row, ordered by how often each part is used: tools first (always in the
    /// same place), then the lane being edited, then options that only exist for the active tool,
    /// then the pointer readout and the "set value" control pinned to the right edge.
    void draw_tool_buttons(bool is_program);
    void draw_source_controls();
    void draw_ramp_options();
    void draw_right_cluster(bool is_program);
#pragma endregion

    /// Small status line while a streamed lane rebuild is running.
    void draw_build_progress();

    /// @param active_track track currently being edited.
    /// @return onion tracks selected by the view settings; never includes @p active_track.
    [[nodiscard]] std::vector<std::uint16_t> onion_tracks(std::uint16_t active_track) const;

    editor::DataEditing* data_editing_ = nullptr;
    std::unique_ptr<editor::EventLane> event_lane_;

    double set_value_ = 0.0;  ///< value typed into the header's "set" box, in the source's units
    int set_value_key_ = -1;  ///< source the box was last reset for, so it starts at that source's default

#pragma region Lane cache
    struct LaneKey {
        std::uint64_t track_revision = 0;
        std::uint64_t data_revision = 0;
        std::uint16_t lane_track = 0;
        std::uint8_t event_source = 0;
        std::uint16_t source_info_key = 0;
        std::uint8_t onion_key = 0;
        bool operator==(const LaneKey&) const = default;
    };

    LaneKey lane_key_{};           ///< key of the points currently readable from the lane
    LaneKey requested_key_{};      ///< key of the build in flight
    bool lane_front_valid_ = false; ///< lane_key_ describes what the lane holds
    bool lane_building_ = false;


    /// @return key covering every input the projection depends on; 0 means "rebuild always".
    [[nodiscard]] LaneKey lane_key() const;
#pragma endregion

#pragma region Program change label cache
    struct ProgramChangeLabel {
        midi::MIDITick tick;
        std::uint8_t channel;                // color is looked up at draw time
        std::optional<std::uint8_t> program; // nullopt = several programs at this tick
    };
    struct PcLabelKey {
        std::uint16_t track = 0;
        std::uint8_t channel = 0;
        std::uint64_t revision = 0;
        bool operator==(const PcLabelKey&) const = default;
    };

    /// Rebuilds pc_labels_ for the active track when track, channel filter or revision changed.
    void refresh_pc_labels();

    std::vector<ProgramChangeLabel> pc_labels_;
    PcLabelKey pc_key_{};
    bool pc_labels_valid_ = false;
#pragma endregion
};

}