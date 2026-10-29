#include "ui/panels/panels.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

#include "ui/app/editor_app.h"
#include "ui/app/main_window.h"
#include "ui/app/managers.h"
#include "ui/rendering/rendering.h"
#include "ui/widgets/toolbar_widgets.h"
#include "ui/app/view_settings.h"
#include "ui/widgets/editor_widget.h"
#include "audio/audio_timer.h"
#include "audio/midi_audio.h"
#include "editor/editor_controller.h"
#include "editor/editing/track_editing.h"
#include "editor/navigation.h"
#include "editor/project/project_manager.h"
#include "editor/timeline/timeline.h"
#include "core/util.h"

// ---------------------------------------------------------------------------
// Navigation / ruler panel group.
//
// Extracted from the former monolithic `app/ui/panels.cpp`. These panels map
// model-space ticks to on-screen positions: the horizontal and vertical scroll
// bars, the data-view strip and the bar-number ruler.
// ---------------------------------------------------------------------------

namespace andromeda::app {

namespace {

constexpr float BAR_NUMBER_FONT_SIZE = 19.0f;

} // namespace

void draw_panel_scroll_navigation(MainWindow& parent) {
    editor::MIDITick last_start = 0;
    {
        std::vector<midi::MIDITrack>& tracks = *parent.controller().get_project_manager()->get_tracks();
        for (midi::MIDITrack& track : tracks) {
            if (!track.get_notes().empty()) {
                last_start = std::max(last_start, track.get_notes().back().start);
            }
        }
    }

    const float tail = static_cast<float>(std::max<std::uint16_t>(parent.get_ppq(), 1)) * 4.0f * 8.0f;
    const float latest = static_cast<float>(last_start) + tail;

    const bool is_track = parent.render_type() == RenderType::TrackView;

    float tick_pos = 0.0f;
    float tick_end = 0.0f;
    if (is_track) {
        std::shared_lock lock(parent.track_navigation()->mutex);
        tick_pos = parent.track_navigation()->value.tick_pos;
        tick_end = tick_pos + parent.track_navigation()->value.zoom_ticks;
    } else {
        std::shared_lock lock(parent.navigation()->mutex);
        tick_pos = parent.navigation()->value.tick_pos;
        tick_end = tick_pos + parent.navigation()->value.zoom_ticks;
    }

    float scrolled = 0.0f;
    bool following = false;
    auto* app = parent.get_editor_app();
    if (app->get_audio_engine()->get_current_engine()->is_playing()) {
        {
            std::lock_guard lock(parent.view_settings_handle()->mutex);
            following = parent.settings().pr_autoscroll;
        }

        if (following) {
            editor::timeline::Timeline* timeline = parent.controller().get_project_manager()->get_timeline();
            const editor::MIDITick now = timeline->secs_to_tick(app->get_timer()->elapsed());
            const editor::MIDITick from = timeline->secs_to_tick(app->get_timer()->get_start_time());
            scrolled = static_cast<float>(now > from ? now - from : 0);
            tick_pos += scrolled;
            tick_end += scrolled;
        }
    }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight() * 0.55f;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const float track_y = p0.y + h * 0.5f;
    const float range = std::max(latest, 1.0f);

    constexpr float RAIL_HALF = 4.0f;
    constexpr float THUMB_HALF = 5.0f;
    constexpr float MIN_THUMB = 22.0f;

    draw->AddRectFilled({p0.x, track_y - RAIL_HALF}, {p0.x + w, track_y + RAIL_HALF},
                        IM_COL32(16, 16, 18, 255), RAIL_HALF);

    float x0 = p0.x + (tick_pos / range) * w;
    float x1 = p0.x + (std::min(tick_end, range) / range) * w;
    if (x1 - x0 < MIN_THUMB) {
        const float centre = (x0 + x1) * 0.5f;
        x0 = centre - MIN_THUMB * 0.5f;
        x1 = centre + MIN_THUMB * 0.5f;
    }
    x0 = std::max(x0, p0.x);
    x1 = std::min(x1, p0.x + w);

    ImGui::InvisibleButton("##scroll_nav", ImVec2(w, ImGui::GetFrameHeight()));

    const auto set_view_pos = [&](float pos) {
        if (is_track) {
            std::unique_lock lock(parent.track_navigation()->mutex);
            parent.track_navigation()->value.tick_pos = pos;
        } else {
            std::unique_lock lock(parent.navigation()->mutex);
            parent.navigation()->value.tick_pos = pos;
        }
    };

    // while playing, dragging looks elsewhere for as long as the button is held; letting go
    // brings the view back to the playhead where it was
    if (ImGui::IsItemActivated()) {
        parent.scroll_return_pos().reset();
        if (following) {
            parent.scroll_return_pos() = tick_pos - scrolled;
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (parent.scroll_return_pos() && following) {
            set_view_pos(*parent.scroll_return_pos());
        }
        parent.scroll_return_pos().reset();
    }

    const bool active = ImGui::IsItemActive();
    const ImU32 thumb = active            ? IM_COL32(122, 122, 130, 255)
                        : ImGui::IsItemHovered() ? IM_COL32(96, 96, 103, 255)
                                                 : IM_COL32(72, 72, 78, 255);
    draw->AddRectFilled({x0, track_y - THUMB_HALF}, {x1, track_y + THUMB_HALF}, thumb,
                        THUMB_HALF);
    if (active) {
        const float mx = ImGui::GetIO().MousePos.x - p0.x;
        const float span = std::max(1.0f, tick_end - tick_pos);
        const float new_pos = std::max(0.0f, (mx / std::max(w, 1.0f)) * range - span * 0.5f);

        // while following playback this is an offset from the playhead and may go below 0;
        // clamping it kept the view from going back before the ground already played
        set_view_pos(following ? new_pos - scrolled : std::max(0.0f, new_pos - scrolled));
    }
}

void draw_panel_scroll_navigation_vertical(MainWindow& parent) {
    // REFACTOR: the panel asked for the TODO "have the controller report the used
    // track count" - it now does, so the panel no longer reaches into TrackEditing.
    const float range = static_cast<float>(parent.controller().get_used_track_count()) + 10.0f;

    float track_pos = 0.0f;
    float zoom_tracks = 0.0f;
    {
        std::shared_lock lock(parent.track_navigation()->mutex);
        track_pos = parent.track_navigation()->value.track_pos;
        zoom_tracks = parent.track_navigation()->value.zoom_tracks;
    }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetContentRegionAvail().y;
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();

    constexpr float RAIL_HALF = 4.0f;
    constexpr float THUMB_HALF = 5.0f;
    constexpr float MIN_THUMB = 22.0f;

    const float cx = p0.x + w * 0.5f;
    draw->AddRectFilled({cx - RAIL_HALF, p0.y}, {cx + RAIL_HALF, p0.y + h},
                        IM_COL32(16, 16, 18, 255), RAIL_HALF);

    float y0 = p0.y + (std::clamp(track_pos, 0.0f, range) / range) * h;
    float y1 = p0.y + (std::clamp(track_pos + zoom_tracks, 0.0f, range) / range) * h;
    if (y1 - y0 < MIN_THUMB) {
        const float centre = (y0 + y1) * 0.5f;
        y0 = centre - MIN_THUMB * 0.5f;
        y1 = centre + MIN_THUMB * 0.5f;
    }
    if (y0 < p0.y) {
        y1 += p0.y - y0;
        y0 = p0.y;
    }
    if (y1 > p0.y + h) {
        y0 -= y1 - (p0.y + h);
        y1 = p0.y + h;
    }

    ImGui::InvisibleButton("##track_scroll", ImVec2(w, h));

    const bool active = ImGui::IsItemActive();
    const ImU32 thumb = active                   ? IM_COL32(122, 122, 130, 255)
                        : ImGui::IsItemHovered() ? IM_COL32(96, 96, 103, 255)
                                                 : IM_COL32(72, 72, 78, 255);
    draw->AddRectFilled({cx - THUMB_HALF, y0}, {cx + THUMB_HALF, y1}, thumb, THUMB_HALF);

    if (active) {
        const float my = ImGui::GetIO().MousePos.y - p0.y;
        const float new_pos =
            std::clamp((my / h) * range - zoom_tracks * 0.5f, 0.0f, range);

        std::unique_lock lock(parent.track_navigation()->mutex);
        parent.track_navigation()->value.track_pos = new_pos;
    }
}

void draw_panel_data_viewer(MainWindow& parent) {
    // read without the lock: only safe because panels and gl pass share the ui thread
    ViewSettings& vs = parent.settings();

    // using new dockable panel system!

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const ImVec2 wmax{ wp.x + ws.x, wp.y + ws.y };

    const float header_h = ImGui::GetFrameHeight() + style.ItemSpacing.y;
    const ImVec2 hole_min{ origin.x + parent.get_keyboard_width(), origin.y + header_h };
    const ImVec2 content_max = ImGui::GetCurrentWindow()->ContentRegionRect.Max;
    const ImVec2 avail{ std::max(1.0f, content_max.x - hole_min.x),
                       std::max(1.0f, content_max.y - hole_min.y) };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = ImGui::GetColorU32(ImGuiCol_WindowBg);
    dl->AddRectFilled(wp, { wmax.x, hole_min.y }, fill);
    dl->AddRectFilled({ wp.x, hole_min.y }, { hole_min.x, wmax.y }, fill);
    if (hole_min.x + avail.x < wmax.x) {
        dl->AddRectFilled({ hole_min.x + avail.x, hole_min.y }, wmax, fill);
    }
    if (hole_min.y + avail.y < wmax.y) {
        dl->AddRectFilled({ hole_min.x, hole_min.y + avail.y }, { hole_min.x + avail.x, wmax.y },
            fill);
    }

    ImGui::SetCursorScreenPos({ hole_min.x, origin.y });
    ImGui::TextUnformatted("Property");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##property", to_string(vs.pr_dataview_state).c_str())) {
        if (ImGui::Selectable("Velocity",
            vs.pr_dataview_state == VS_PianoRoll_DataViewState::NoteVelocities)) {
            vs.pr_dataview_state = VS_PianoRoll_DataViewState::NoteVelocities;
        }
        if (ImGui::Selectable("Pitch Bend",
            vs.pr_dataview_state == VS_PianoRoll_DataViewState::PitchBend)) {
            vs.pr_dataview_state = VS_PianoRoll_DataViewState::PitchBend;
        }
        ImGui::EndCombo();
    }

    // the widget owns the strip; this panel only decides where it goes and when its pass runs
    EditorWidget& data_view = parent.data_view();
    data_view.context.work_rect = editor::ViewRect{ hole_min.x, hole_min.y, avail.x, avail.y };
    dl->AddCallback([](const ImDrawList*, const ImDrawCmd* cmd) {
        static_cast<EditorWidget*>(cmd->UserCallbackData)->draw_gl();
        }, &data_view);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    ImGui::SetCursorScreenPos(hole_min);
    ImGui::Dummy(avail);

    data_view.update();
}

void draw_panel_bar_numbers(MainWindow& parent) {

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float avail_w = ImGui::GetContentRegionAvail().x;

    const float origin = ImGui::GetWindowPos().x;
    const float full_w = ImGui::GetWindowSize().x;
    const float space_alloc = parent.get_keyboard_width();
    const float roll_w = std::max(0.0f, full_w - space_alloc);
    const float label_gap = ImGui::GetStyle().WindowPadding.x;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect({origin, p0.y}, {origin + full_w, p0.y + BAR_NUMBER_FONT_SIZE * 2.0f},
                       false);

    std::size_t bar_num = 0;
    editor::MIDITick curr_bar_tick = 0;

    const app::TickView tick_view = parent.get_central_tick_view();
    const auto min_tick = tick_view.pos;
    const auto max_tick = tick_view.pos + tick_view.zoom;
    const auto* timeline = parent.controller().get_project_manager()->get_timeline();

    timeline->for_each_time_signature(min_tick, max_tick,
        [&](const editor::timeline::TimeSigPoint& s, std::optional<midi::MIDITick> next) {
            const double seg_end = next ? (double)(*next) : (double)(max_tick + 1);

            // first bar that can be visible: the one containing view_start (or the segment's first)
            const double lo = std::max((double)min_tick, (double)s.tick);
            midi::MIDITick k = midi::MIDITick((lo - s.tick) / s.bar_len);

            const double pixels_per_bar = static_cast<double>(s.bar_len) / static_cast<double>(tick_view.zoom) * roll_w;
            const float min_label_spacing = 40.0f;

            std::size_t skip_interval = 1;

            while (pixels_per_bar * skip_interval < min_label_spacing)
                skip_interval *= 2;

            for (;; ++k) {
                const double bar_t = double(s.tick) + double(k) * s.bar_len;
                if (bar_t >= seg_end || bar_t > (double)max_tick) break;

                const float ui_pos =
                    (float)(bar_t > (double)min_tick ? (bar_t - (double)min_tick) / (double)tick_view.zoom : 0)
                    * roll_w + space_alloc + label_gap;

                const std::size_t bar_number = static_cast<std::size_t>(s.first_bar) + k + 1;
                char label[16];
                std::snprintf(label, sizeof(label), "%zu", bar_number);

                if ((bar_number & (skip_interval - 1)) != 0) continue;

                draw->AddText(ImGui::GetFont(), BAR_NUMBER_FONT_SIZE, { origin + ui_pos, p0.y },
                    IM_COL32(160, 160, 160, 255), label);

                bar_num++;
                if (bar_num > 4096) break;
            }
        });

    draw->PopClipRect();

    ImGui::Dummy(ImVec2(avail_w, BAR_NUMBER_FONT_SIZE));
}

} // namespace andromeda::app
