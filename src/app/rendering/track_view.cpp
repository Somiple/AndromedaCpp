#include "app/rendering/track_view.h"
#include "app/main_window.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>

#include "app/rendering/piano_roll.h"
#include "util/numeric.h"

namespace andromeda::app::rendering {

TrackViewRenderer::TrackViewRenderer(MainWindow* app) : Renderer(app) {
    tv_program_ = ShaderProgram::create_from_files("./assets/shaders/track_view_bg");
    tv_notes_program_ = ShaderProgram::create_from_files("./assets/shaders/track_view_note");

    tv_vertex_buffer_ = Buffer(GL_ARRAY_BUFFER);
    tv_vertex_buffer_.set_data(std::span<const Vertex>(QUAD_VERTICES), GL_STATIC_DRAW);

    tv_index_buffer_ = Buffer(GL_ELEMENT_ARRAY_BUFFER);
    tv_index_buffer_.set_data(std::span<const std::uint32_t>(TV_QUAD_INDICES), GL_STATIC_DRAW);

    tv_vertex_array_ = VertexArray::create();
    const GLint pos_attrib = tv_program_.get_attrib_location("vPos");
    SET_ATTRIBUTE(GL_FLOAT, tv_vertex_array_, pos_attrib, Vertex, position);

    tv_instance_buffer_ = Buffer(GL_ARRAY_BUFFER);
    bars_render_.assign(BAR_BUFFER_SIZE, RenderTrackViewBar{0.0f, 1.0f, 0});
    tv_instance_buffer_.set_data(std::span<const RenderTrackViewBar>(bars_render_),
                                 GL_DYNAMIC_DRAW);

    const GLint tv_bar_start = tv_program_.get_attrib_location("barStart");
    SET_ATTRIBUTE(GL_FLOAT, tv_vertex_array_, tv_bar_start, RenderTrackViewBar, bar_start);
    const GLint tv_bar_length = tv_program_.get_attrib_location("barLength");
    SET_ATTRIBUTE(GL_FLOAT, tv_vertex_array_, tv_bar_length, RenderTrackViewBar, bar_length);
    const GLint tv_bar_meta = tv_program_.get_attrib_location("barMeta");
    SET_ATTRIBUTE(GL_UNSIGNED_INT, tv_vertex_array_, tv_bar_meta, RenderTrackViewBar, bar_meta);

    glVertexAttribDivisor(1, 1);
    glVertexAttribDivisor(2, 1);
    glVertexAttribDivisor(3, 1);

    tv_notes_vbo_ = Buffer(GL_ARRAY_BUFFER);
    tv_notes_vbo_.set_data(std::span<const Vertex>(QUAD_VERTICES), GL_STATIC_DRAW);

    tv_notes_ebo_ = Buffer(GL_ELEMENT_ARRAY_BUFFER);
    tv_notes_ebo_.set_data(std::span<const std::uint32_t>(TV_QUAD_INDICES), GL_STATIC_DRAW);

    tv_notes_vao_ = VertexArray::create();
    SET_ATTRIBUTE(GL_FLOAT, tv_notes_vao_, 0, Vertex, position);

    tv_notes_ibo_ = Buffer(GL_ARRAY_BUFFER);
    notes_render_.assign(TV_NOTE_BUFFER_SIZE, RenderTrackViewNote{{0.0f, 1.0f, 0.0f, 1.0f}, 0});
    tv_notes_ibo_.set_data(std::span<const RenderTrackViewNote>(notes_render_), GL_DYNAMIC_DRAW);

    const GLint tv_note_rect = tv_notes_program_.get_attrib_location("noteRect");
    SET_ATTRIBUTE(GL_FLOAT, tv_notes_vao_, tv_note_rect, RenderTrackViewNote, note_rect);
    const GLint tv_note_meta = tv_notes_program_.get_attrib_location("noteMeta");
    SET_ATTRIBUTE(GL_UNSIGNED_INT, tv_notes_vao_, tv_note_meta, RenderTrackViewNote, note_meta);

    glVertexAttribDivisor(1, 1);
    glVertexAttribDivisor(2, 1);

    {
        auto* all_tracks = _app->editor_controller.get_project_manager()->get_tracks();
        last_note_start_.assign(all_tracks->size(), 0);
        first_render_note_.assign(all_tracks->size(), 0);
    }
}

float TrackViewRenderer::get_time() const {
    // TODO: change once main window's audio subsystem has been refactored
    ViewSettings* view_settings = &_app->view_settings->value;
    if (view_settings->pr_autoscroll) {
        auto* playback_manager = _app->get_playback_manager();
        if (playback_manager->is_playing()) {
            return static_cast<float>(playback_manager->get_playback_ticks());
        }
        return _app->track_nav->value.tick_pos_smoothed;
    }
    return _app->track_nav->value.tick_pos_smoothed;
}

void TrackViewRenderer::draw() {
    if (!render_active_) {
        return;
    }

    const float tick_pos = get_time();

    float zoom_ticks = 0.0f;
    float track_pos = 0.0f;
    float zoom_tracks = 0.0f;
    {
        auto& navigation = _app->track_nav;
        zoom_ticks = navigation->value.zoom_ticks_smoothed;
        track_pos = navigation->value.track_pos_smoothed;
        zoom_tracks = navigation->value.zoom_tracks_smoothed;
    }

    float view_offset = 0.0f;
    {
        auto* playback_manager = _app->get_playback_manager();
        bool autoscroll = _app->view_settings->value.pr_autoscroll;

        // fixed rust bug: taken once at play start and scaled on zoom, so it drifted from the
        // playhead line; it is now the same live formula
        if (playback_manager->is_playing() && autoscroll) {
            auto& navigation = _app->track_nav;
            view_offset = navigation->value.tick_pos_smoothed -
                          static_cast<float>(playback_manager->get_playback_start_tick());
        }
    }

    // the view on screen stops at the song start, as the playhead line's does
    const float tick_pos_offs = std::max(0.0f, tick_pos + view_offset);

    {
        glUseProgram(tv_program_.id());

        int curr_track = 0;

        tv_vertex_array_.bind();
        tv_instance_buffer_.bind();
        tv_vertex_buffer_.bind();
        tv_index_buffer_.bind();

        while (static_cast<float>(curr_track) < track_pos + zoom_tracks) {
            float curr_bar_tick = 0.0f;
            std::size_t bar_num = 0;
            std::size_t bar_id = 0;

            const float num_bars = zoom_tracks;

            const float bar_top = (zoom_tracks - static_cast<float>(curr_track)) + track_pos;
            const float bar_bottom =
                (zoom_tracks - static_cast<float>(curr_track) - 1.0f) + track_pos;

            tv_program_.set_float("width", window_size_.x);
            tv_program_.set_float("height", window_size_.y);
            tv_program_.set_float("tvBarTop", bar_top / num_bars);
            tv_program_.set_float("tvBarBottom", bar_bottom / num_bars);
            {
                // TODO: use controller->get_active_track(); instead
                auto* controller = &_app->editor_controller;
                tv_program_.set_float("ppqNorm", static_cast<float>(controller->get_project_manager()->get_ppq()) / zoom_ticks);
                tv_program_.set_int("currTrack", static_cast<int>(controller->get_active_track()));
            }

            auto& bar_cacher = _app->bar_cacher;
            while (curr_bar_tick <= zoom_ticks + tick_pos_offs) {
                const auto [bar_tick, bar_length] = bar_cacher->get_bar_interval(bar_num);

                if (bar_length == 0) {
                    break;
                }

                if (static_cast<float>(bar_tick + bar_length) < tick_pos_offs) {
                    curr_bar_tick += static_cast<float>(bar_length);
                    bar_num += 1;
                    continue;
                }

                bars_render_[bar_id] = RenderTrackViewBar{
                    (curr_bar_tick - tick_pos_offs) / zoom_ticks,
                    static_cast<float>(bar_length) / zoom_ticks,
                    ((static_cast<std::uint32_t>(bar_num) & 0b1u) << 31) |
                        (static_cast<std::uint32_t>(curr_track) & 0xFFFFu)};

                bar_id += 1;
                if (bar_id >= BAR_BUFFER_SIZE) {
                    tv_instance_buffer_.set_data(std::span<const RenderTrackViewBar>(bars_render_),
                                                 GL_DYNAMIC_DRAW);
                    glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                            static_cast<GLsizei>(BAR_BUFFER_SIZE));
                    bar_id = 0;
                }

                curr_bar_tick += static_cast<float>(bar_length);
                bar_num += 1;
            }

            if (bar_id != 0) {
                tv_instance_buffer_.set_data(std::span<const RenderTrackViewBar>(bars_render_),
                                             GL_DYNAMIC_DRAW);
                glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                        static_cast<GLsizei>(bar_id));
            }

            curr_track += 1;
        }

        glUseProgram(0);
    }

    {
        glUseProgram(tv_notes_program_.id());

        glActiveTexture(GL_TEXTURE0);
        _app->note_colors->get_texture().bind();

        tv_notes_program_.set_float("width", window_size_.x);
        tv_notes_program_.set_float("height", window_size_.y);
        tv_notes_program_.set_float("zoomTicks", 1.0f / zoom_ticks);
        tv_notes_program_.set_float("zoomTracks", 1.0f / zoom_tracks);

        std::vector<midi::MIDITrack>& all_tracks = *_app->editor_controller.get_project_manager()->get_tracks();

        tv_notes_vao_.bind();
        tv_notes_ibo_.bind();
        tv_notes_vbo_.bind();
        tv_notes_ebo_.bind();

        if (last_note_start_.size() != all_tracks.size()) {
            last_note_start_.assign(all_tracks.size(), 0);
            first_render_note_.assign(all_tracks.size(), 0);
        }

        const auto total_tracks = static_cast<std::ptrdiff_t>(all_tracks.size());
        const auto track_start = static_cast<std::size_t>(
            std::clamp(util::saturating_cast<std::ptrdiff_t>(std::ceil(track_pos)) - 1,
                       static_cast<std::ptrdiff_t>(0), total_tracks));
        const auto track_end = static_cast<std::size_t>(
            std::clamp(util::saturating_cast<std::ptrdiff_t>(std::ceil(track_pos + zoom_tracks)),
                       static_cast<std::ptrdiff_t>(0), total_tracks));

        std::size_t note_id = 0;
        std::size_t curr_track = track_start;

        for (std::size_t t = track_start; t < track_end; ++t) {
            const std::vector<midi::Note>& notes = all_tracks[t].get_notes();
            if (notes.empty()) {
                curr_track += 1;
                continue;
            }

            std::size_t n_off = first_render_note_[curr_track];
            if (last_time_ > tick_pos_offs) {
                if (n_off == 0) {
                    for (const midi::Note& note : notes) {
                        if (static_cast<float>(note.end()) > tick_pos_offs) {
                            break;
                        }
                        n_off += 1;
                    }
                } else {
                    if (n_off > notes.size()) {
                        first_render_note_[curr_track] = 0;
                        last_note_start_[curr_track] = 0;
                        n_off = notes.size();
                    }

                    for (std::size_t i = n_off; i-- > 0;) {
                        if (static_cast<float>(notes[i].end()) <= tick_pos_offs) {
                            break;
                        }
                        n_off -= 1;
                    }
                }
                first_render_note_[curr_track] = n_off;
            } else if (last_time_ < tick_pos_offs) {
                for (std::size_t i = n_off; i < notes.size(); ++i) {
                    if (static_cast<float>(notes[i].end()) > tick_pos_offs) {
                        break;
                    }
                    n_off += 1;
                }
                first_render_note_[curr_track] = n_off;
            }

            n_off = std::min(n_off, notes.size());
            const auto first = notes.begin() + static_cast<std::ptrdiff_t>(n_off);
            const auto part = std::partition_point(first, notes.end(), [&](const midi::Note& note) {
                return static_cast<float>(note.get_start()) <= tick_pos_offs + zoom_ticks;
            });
            const std::size_t note_end = n_off + static_cast<std::size_t>(part - first);

            static const std::vector<std::size_t> EMPTY;
            editor::SharedSelectedNotes* selection = _app->editor_controller.get_selection();
            const std::vector<std::size_t>* sel_ptr =
                selection ? selection->get_selected_ids_in_track(
                                static_cast<std::uint16_t>(curr_track))
                          : nullptr;
            const std::vector<std::size_t>& sel_ids = sel_ptr != nullptr ? *sel_ptr : EMPTY;

            std::size_t sel_idx = 0;
            std::size_t note_idx = n_off;

            for (std::size_t i = n_off; i < note_end; ++i) {
                const midi::Note& note = notes[i];
                {
                    const float note_top =
                        (zoom_tracks - static_cast<float>(curr_track) -
                         (1.0f - ((static_cast<float>(note.key) + 1.0f) / 128.0f))) + track_pos;
                    const float note_bottom =
                        (zoom_tracks - static_cast<float>(curr_track) -
                         (1.0f - ((static_cast<float>(note.key) - 1.0f) / 128.0f))) + track_pos;

                    const std::size_t trk_chan =
                        (curr_track << 4) | static_cast<std::size_t>(note.get_channel());

                    std::uint32_t note_meta =
                        static_cast<std::uint32_t>(_app->note_colors->get_index(trk_chan));

                    if (sel_idx < sel_ids.size() && note_idx == sel_ids[sel_idx]) {
                        note_meta |= 1u << 13;
                        sel_idx += 1;
                    }

                    notes_render_[note_id] =
                        RenderTrackViewNote{{static_cast<float>(note.start) - tick_pos_offs,
                                             static_cast<float>(note.length), note_bottom,
                                             note_top},
                                            note_meta};

                    note_id += 1;
                    note_idx += 1;

                    if (note_id >= TV_NOTE_BUFFER_SIZE) {
                        tv_notes_ibo_.set_sub_data(
                            0, std::span<const RenderTrackViewNote>(notes_render_));
                        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                                static_cast<GLsizei>(TV_NOTE_BUFFER_SIZE));
                        note_id = 0;
                    }
                }
            }

            curr_track += 1;
        }

        {
            std::lock_guard ghost_lock(ghost_notes_->mutex);
            std::shared_lock offset_lock(ghost_notes_render_offset_->mutex);
            const editor::SignedMIDITrkVec& offset = ghost_notes_render_offset_->value;

            for (const auto& [track_id, notes] : ghost_notes_->value) {
                const auto shifted =
                    static_cast<editor::SignedMIDITrk>(track_id) + offset.second;
                if (shifted < 0) {
                    continue;
                }
                const auto ghost_track = static_cast<std::size_t>(shifted);

                for (const midi::Note& note : notes) {
                    const float note_top =
                        (zoom_tracks - static_cast<float>(ghost_track) -
                         (1.0f - ((static_cast<float>(note.key) + 1.0f) / 128.0f))) + track_pos;
                    const float note_bottom =
                        (zoom_tracks - static_cast<float>(ghost_track) -
                         (1.0f - ((static_cast<float>(note.key) - 1.0f) / 128.0f))) + track_pos;
                    const float note_left =
                        static_cast<float>(static_cast<editor::SignedMIDITick>(note.start) +
                                           offset.first) -
                        tick_pos_offs;

                    const std::size_t trk_chan =
                        (ghost_track << 4) | static_cast<std::size_t>(note.get_channel());

                    notes_render_[note_id] = RenderTrackViewNote{
                        {note_left, static_cast<float>(note.length), note_bottom, note_top},
                        static_cast<std::uint32_t>(_app->note_colors->get_index(trk_chan))};

                    note_id += 1;

                    if (note_id >= TV_NOTE_BUFFER_SIZE) {
                        tv_notes_ibo_.set_sub_data(
                            0, std::span<const RenderTrackViewNote>(notes_render_));
                        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                                static_cast<GLsizei>(TV_NOTE_BUFFER_SIZE));
                        note_id = 0;
                    }
                }
            }
        }

        if (note_id != 0) {
            tv_notes_ibo_.set_sub_data(
                0, std::span<const RenderTrackViewNote>(notes_render_).first(note_id));
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                    static_cast<GLsizei>(note_id));
        }

        glUseProgram(0);
    }

    last_time_ = tick_pos_offs;
}

}
