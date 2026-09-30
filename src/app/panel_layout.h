#pragma once

#include <imgui.h>

#include <string>
#include <unordered_map>

namespace andromeda::app {

class PanelLayout {
public:
    void begin_frame(ImVec2 pos, ImVec2 size);

    bool top(const char* id, float default_extent, bool auto_size = true,
             ImGuiWindowFlags extra_flags = 0);
    bool bottom(const char* id, float default_extent, bool auto_size = true,
                ImGuiWindowFlags extra_flags = 0);
    bool left(const char* id, float default_extent, bool auto_size = true,
              ImGuiWindowFlags extra_flags = 0);
    bool right(const char* id, float default_extent, bool auto_size = true,
               ImGuiWindowFlags extra_flags = 0);

    void end_panel();

    [[nodiscard]] ImVec2 central_pos() const { return {rect_min_.x, rect_min_.y}; }
    [[nodiscard]] ImVec2 central_size() const {
        return {rect_max_.x - rect_min_.x, rect_max_.y - rect_min_.y};
    }

    [[nodiscard]] bool pointer_over_panel() const { return pointer_over_panel_; }

    void set_separator_enabled(bool enabled) { separator_enabled_ = enabled; }

private:
    enum class Side { Top, Bottom, Left, Right };

    bool begin_panel(const char* id, Side side, float default_extent, bool auto_size,
                     ImGuiWindowFlags extra_flags);

    struct PanelState {
        float extent = 0.0f;
        bool measured = false;
        float content = 0.0f;
        bool content_measured = false;
        float min_extent = 0.0f;
    };

    ImVec2 rect_min_{};
    ImVec2 rect_max_{};

    std::unordered_map<std::string, PanelState> states_;

    std::string open_id_;
    Side open_side_ = Side::Top;
    bool open_auto_size_ = true;
    float open_center_offset_ = 0.0f;
    bool open_ = false;

    bool separator_enabled_ = true;
    bool pointer_over_panel_ = false;
};

}
