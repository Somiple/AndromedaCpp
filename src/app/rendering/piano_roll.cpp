#include "app/rendering/piano_roll.h"
#include "app/main_window.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <utility>

#include "editor/settings/editor_settings.h"
#include "util/debugger.h"
#include "util/numeric.h"

namespace andromeda::app::rendering {

using util::Debugger;

PianoRollRenderer::PianoRollRenderer(app::MainWindow* app) : Renderer(app) {
    pr_program_ = ShaderProgram::create_from_files("./assets/shaders/piano_roll_bg");
    pr_notes_program_ = ShaderProgram::create_from_files("./assets/shaders/piano_roll_note");
    pr_keyboard_program_ = ShaderProgram::create_from_files("./assets/shaders/piano_roll_kb");

    pr_vertex_buffer_ = Buffer(GL_ARRAY_BUFFER);
    pr_vertex_buffer_.set_data(std::span<const Vertex>(QUAD_VERTICES), GL_STATIC_DRAW);

    pr_index_buffer_ = Buffer(GL_ELEMENT_ARRAY_BUFFER);
    pr_index_buffer_.set_data(std::span<const std::uint32_t>(QUAD_INDICES), GL_STATIC_DRAW);

    pr_vertex_array_ = VertexArray::create();
    pr_instance_buffer_ = Buffer(GL_ARRAY_BUFFER);
    bars_render_.assign(32, RenderPianoRollBar{0.0f, 1.0f, 0});
    pr_instance_buffer_.set_data(std::span<const RenderPianoRollBar>(bars_render_),
                                 GL_DYNAMIC_DRAW);

    const GLint pr_bar_start = pr_program_.get_attrib_location("barStart");
    SET_ATTRIBUTE(GL_FLOAT, pr_vertex_array_, pr_bar_start, RenderPianoRollBar, bar_start);
    const GLint pr_bar_length = pr_program_.get_attrib_location("barLength");
    SET_ATTRIBUTE(GL_FLOAT, pr_vertex_array_, pr_bar_length, RenderPianoRollBar, bar_length);
    const GLint pr_bar_number = pr_program_.get_attrib_location("barNumber");
    SET_ATTRIBUTE(GL_UNSIGNED_INT, pr_vertex_array_, pr_bar_number, RenderPianoRollBar, bar_number);

    glVertexAttribDivisor(0, 1);
    glVertexAttribDivisor(1, 1);
    glVertexAttribDivisor(2, 1);

    pr_notes_vbo_ = Buffer(GL_ARRAY_BUFFER);
    pr_notes_vbo_.set_data(std::span<const Vertex>(QUAD_VERTICES), GL_STATIC_DRAW);

    pr_notes_ebo_ = Buffer(GL_ELEMENT_ARRAY_BUFFER);
    pr_notes_ebo_.set_data(std::span<const std::uint32_t>(QUAD_INDICES), GL_STATIC_DRAW);

    pr_notes_vao_ = VertexArray::create();
    SET_ATTRIBUTE(GL_FLOAT, pr_notes_vao_, 0, Vertex, position);

    pr_notes_ibo_ = Buffer(GL_ARRAY_BUFFER);
    notes_render_.assign(NOTE_BUFFER_SIZE, RenderPianoRollNote{{0.0f, 1.0f, 0.0f, 1.0f}, 0});
    pr_notes_ibo_.set_data(std::span<const RenderPianoRollNote>(notes_render_), GL_DYNAMIC_DRAW);

    const GLint pr_note_rect = pr_notes_program_.get_attrib_location("noteRect");
    SET_ATTRIBUTE(GL_FLOAT, pr_notes_vao_, pr_note_rect, RenderPianoRollNote, note_rect);
    const GLint pr_note_meta = pr_notes_program_.get_attrib_location("noteMeta");
    SET_ATTRIBUTE(GL_UNSIGNED_INT, pr_notes_vao_, pr_note_meta, RenderPianoRollNote, note_meta);

    glVertexAttribDivisor(1, 1);
    glVertexAttribDivisor(2, 1);

    pr_notes_direct_program_ =
        ShaderProgram::create_from_files("./assets/shaders/piano_roll_note_direct");

    pr_notes_direct_vao_ = VertexArray::create();

    {
        constexpr std::array<std::uint32_t, 1> seed{0};
        note_index_buffer_.set_data(std::span<const std::uint32_t>(seed), GL_STREAM_DRAW);
        note_index_capacity_ = seed.size();
        note_index_head_ = seed.size();

        glGenTextures(1, &note_index_tex_);
        glBindTexture(GL_TEXTURE_BUFFER, note_index_tex_);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, note_index_buffer_.id());
        glBindTexture(GL_TEXTURE_BUFFER, 0);
    }

    pr_keyboard_vertex_buffer_ = Buffer(GL_ARRAY_BUFFER);
    pr_keyboard_vertex_buffer_.set_data(std::span<const Vertex>(QUAD_VERTICES), GL_STATIC_DRAW);

    pr_keyboard_index_buffer_ = Buffer(GL_ELEMENT_ARRAY_BUFFER);
    pr_keyboard_index_buffer_.set_data(std::span<const std::uint32_t>(QUAD_INDICES),
                                       GL_STATIC_DRAW);

    pr_keyboard_vertex_array_ = VertexArray::create();
    SET_ATTRIBUTE(GL_FLOAT, pr_keyboard_vertex_array_, 0, Vertex, position);

    pr_keyboard_instance_buffer_ = Buffer(GL_ARRAY_BUFFER);
    kb_render_.fill(RenderPianoRollKeyboard{0, 0});
    pr_keyboard_instance_buffer_.set_data(
        std::span<const RenderPianoRollKeyboard>(kb_render_), GL_DYNAMIC_DRAW);

    const GLint pr_kb_meta0 = pr_keyboard_program_.get_attrib_location("kbMeta0");
    SET_ATTRIBUTE(GL_UNSIGNED_INT, pr_keyboard_vertex_array_, pr_kb_meta0,
                  RenderPianoRollKeyboard, meta0);

    glVertexAttribDivisor(0, 1);
    glVertexAttribDivisor(1, 1);

    note_occlusion_.attach(app->editor_controller.get_project_manager()->get_tracks());

    std::vector<std::size_t> b;
    b.reserve(53);
    std::vector<std::size_t> w;
    w.reserve(75);

    for (std::size_t key = 0; key < 128; ++key) {
        if (is_black(key)) {
            b.push_back(key);
            key_metas_[key].is_black = true;
        } else {
            w.push_back(key);
        }
        key_metas_[key].key = static_cast<std::uint8_t>(key);
    }

    key_ids_ = std::move(w);
    key_ids_.insert(key_ids_.end(), b.begin(), b.end());

    keyboard_height = editor::PR_KEYBOARD_WIDTH;
}

PianoRollRenderer::~PianoRollRenderer() {
    if (note_index_tex_ != 0) {
        glDeleteTextures(1, &note_index_tex_);
        note_index_tex_ = 0;
    }
}

std::size_t PianoRollRenderer::upload_note_indices(const std::vector<std::uint32_t>& ids) {
    constexpr std::size_t RING_TEXELS = 4u << 20;

    const std::size_t need = ids.size();
    const bool grow = note_index_capacity_ < std::max(RING_TEXELS, need);
    const bool wrap = note_index_head_ + need > note_index_capacity_;

    if (grow || wrap) {
        note_index_capacity_ = std::max({note_index_capacity_, RING_TEXELS, need});
        note_index_buffer_.orphan_and_set(std::span<const std::uint32_t>{},
                                          note_index_capacity_ * sizeof(std::uint32_t));
        note_index_head_ = 0;

        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_BUFFER, note_index_tex_);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, note_index_buffer_.id());
        glActiveTexture(GL_TEXTURE0);
    }

    const std::size_t at = note_index_head_;
    note_index_buffer_.set_sub_data_unsynchronized(at * sizeof(std::uint32_t),
                                                   std::span<const std::uint32_t>(ids));
    note_index_head_ += need;
    return at;
}

float PianoRollRenderer::get_time() const {
    // TODO: change once main window's audio subsystem has been refactored
    ViewSettings* view_settings = &_app->view_settings->value;
    if (view_settings->pr_autoscroll) {
        auto* playback_manager = _app->get_playback_manager();
        if (playback_manager->is_playing()) {
            return static_cast<float>(playback_manager->get_playback_ticks());
        }
        return _app->nav->value.tick_pos_smoothed;
    }

    return _app->nav->value.tick_pos_smoothed;
}

void PianoRollRenderer::draw() {
    if (!render_active_) {
        return;
    }

    instances_drawn_ = 0;
    resident_instances_ = 0;
    cpu_instances_ = 0;

    const float tick_pos = get_time();

    float zoom_ticks = 0.0f;
    float key_pos = 0.0f;
    float zoom_keys = 0.0f;
    std::uint16_t nav_curr_track = 0;
    float tick_pos_smoothed = 0.0f;
    {
        editor::PianoRollNavigation& nav = _app->nav->value;
        zoom_ticks = nav.zoom_ticks_smoothed;
        key_pos = nav.key_pos_smoothed;
        zoom_keys = nav.zoom_keys_smoothed;
        nav_curr_track = nav.curr_track;
        tick_pos_smoothed = nav.tick_pos_smoothed;
    }

    bool is_playing = false;
    float playback_pos = 0.0f;
    float view_offset = 0.0f;
    {
        auto* playback_manager = _app->get_playback_manager();
        bool autoscroll = _app->view_settings->value.pr_autoscroll;

        is_playing = playback_manager->is_playing();
        playback_pos = static_cast<float>(playback_manager->get_playback_ticks());
        // fixed rust bug: the offset was taken once at play start and scaled on zoom, so it
        // drifted from the playhead line and bar numbers; it is now the same live formula
        if (is_playing && autoscroll) {
            view_offset = tick_pos_smoothed -
                          static_cast<float>(playback_manager->get_playback_start_tick());
        }
    }

    // the view on screen stops at the song start, as the playhead line's does
    const float tick_pos_offs = std::max(0.0f, tick_pos + view_offset);

    {
        glUseProgram(pr_program_.id());

        float curr_bar_tick = 0.0f;
        std::size_t bar_id = 0;
        std::size_t bar_num = 0;
        {
            const float key_start = key_pos;
            const float key_end = key_pos + zoom_keys;

            pr_program_.set_float("prBarBottom", -key_start / (key_end - key_start));
            pr_program_.set_float("prBarTop", (128.0f - key_start) / (key_end - key_start));
            pr_program_.set_float("width", window_size_.x);
            pr_program_.set_float("height", window_size_.y);
            {
                uint16_t ppq = _app->editor_controller.get_project_manager()->get_ppq();
                pr_program_.set_float("ppqNorm", static_cast<float>(ppq) / zoom_ticks);
            }
            pr_program_.set_float("keyZoom", zoom_keys / 128.0f);
            pr_program_.set_float("keyboardHeight", keyboard_height * keyboard_scale_);

            pr_vertex_array_.bind();
            pr_instance_buffer_.bind();
            pr_vertex_buffer_.bind();
            pr_index_buffer_.bind();

            auto& bar_cacher = _app->bar_cacher;
            while (curr_bar_tick < zoom_ticks + tick_pos_offs) {
                const auto [bar_tick, bar_length] = bar_cacher->get_bar_interval(bar_num);

                if (bar_length == 0) {
                    break;
                }

                if (static_cast<float>(bar_tick + bar_length) < tick_pos_offs) {
                    curr_bar_tick += static_cast<float>(bar_length);
                    bar_num += 1;
                    continue;
                }

                bars_render_[bar_id] =
                    RenderPianoRollBar{(curr_bar_tick - tick_pos_offs) / zoom_ticks,
                                       static_cast<float>(bar_length) / zoom_ticks,
                                       static_cast<std::uint32_t>(bar_num)};
                bar_id += 1;
                if (bar_id >= 32) {
                    pr_instance_buffer_.set_data(std::span<const RenderPianoRollBar>(bars_render_),
                                                 GL_DYNAMIC_DRAW);
                    glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, 32);
                    bar_id = 0;
                }

                curr_bar_tick += static_cast<float>(bar_length);
                bar_num += 1;
            }
        }

        if (bar_id != 0) {
            pr_instance_buffer_.set_data(std::span<const RenderPianoRollBar>(bars_render_),
                                         GL_DYNAMIC_DRAW);
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                    static_cast<GLsizei>(bar_id));
        }

        glUseProgram(0);
    }

    {
        glUseProgram(pr_notes_program_.id());

        {
            std::vector<midi::MIDITrack>& all_tracks = *_app->editor_controller.get_project_manager()->get_tracks();
            // fixed rust bug: returned here with no tracks, so the keyboard was never drawn

            glActiveTexture(GL_TEXTURE0);
            _app->note_colors->get_texture().bind();

            pr_notes_program_.set_int("noteColorTexture", 0);
            pr_notes_program_.set_float("width", window_size_.x);
            pr_notes_program_.set_float("height", window_size_.y);
            pr_notes_program_.set_float("keyboardHeight", keyboard_height * keyboard_scale_);

            {
                ViewSettings* view_settings = &_app->view_settings->value;
                VS_PianoRoll_OnionState onion_state = view_settings->pr_onion_state;
                VS_PianoRoll_OnionColoring onion_coloring = view_settings->pr_onion_coloring;

                const auto cur = static_cast<std::size_t>(nav_curr_track);
                std::size_t iter_begin = cur;
                std::size_t iter_end = cur;

                switch (onion_state) {
                case VS_PianoRoll_OnionState::NoOnion:
                    break;
                case VS_PianoRoll_OnionState::ViewAll:
                    iter_begin = 0;
                    iter_end = all_tracks.size();
                    break;
                case VS_PianoRoll_OnionState::ViewPrevious:
                    if (cur != 0) {
                        iter_begin = cur - 1;
                        iter_end = cur + 1;
                    }
                    break;
                case VS_PianoRoll_OnionState::ViewNext:
                    if (cur != all_tracks.size() - 1) {
                        iter_begin = cur;
                        iter_end = cur + 2;
                    }
                    break;
                }

                iter_begin = std::min(iter_begin, all_tracks.size());
                iter_end = std::min(iter_end, all_tracks.size());

                std::uint32_t onion_track_color_meta = 0;
                switch (onion_coloring) {
                case VS_PianoRoll_OnionColoring::FullColor:    onion_track_color_meta = 0b00; break;
                case VS_PianoRoll_OnionColoring::PartialColor: onion_track_color_meta = 0b01; break;
                case VS_PianoRoll_OnionColoring::GrayedOut:    onion_track_color_meta = 0b10; break;
                }

                {
                    std::size_t note_id = 0;

                    note_gpu_cache_->begin_frame();

                    int color_mode = 0;
                    switch (_app->note_colors->get_index_type()) {
                    case NoteColorIndexing::Channel:      color_mode = 0; break;
                    case NoteColorIndexing::Track:        color_mode = 1; break;
                    case NoteColorIndexing::ChannelTrack: color_mode = 2; break;
                    }

                    glUseProgram(pr_notes_direct_program_.id());
                    pr_notes_direct_program_.set_int("noteColorTexture", 0);
                    pr_notes_direct_program_.set_int("selectedBits", 1);
                    pr_notes_direct_program_.set_int("noteData", 2);
                    pr_notes_direct_program_.set_int("noteIndex", 3);
                    pr_notes_direct_program_.set_int("indexed", 0);
                    glActiveTexture(GL_TEXTURE3);
                    glBindTexture(GL_TEXTURE_BUFFER, note_index_tex_);
                    glActiveTexture(GL_TEXTURE0);
                    pr_notes_direct_program_.set_float("width", window_size_.x);
                    pr_notes_direct_program_.set_float("height", window_size_.y);
                    pr_notes_direct_program_.set_float("keyboardHeight",
                                                       keyboard_height * keyboard_scale_);
                    pr_notes_direct_program_.set_float("tickPos", tick_pos_offs);
                    pr_notes_direct_program_.set_float("zoomTicks", zoom_ticks);
                    pr_notes_direct_program_.set_float("keyPos", key_pos);
                    pr_notes_direct_program_.set_float("zoomKeys", zoom_keys);
                    pr_notes_direct_program_.set_float("playbackPos", playback_pos);
                    pr_notes_direct_program_.set_float("highlightSize", zoom_ticks * 0.001f);
                    pr_notes_direct_program_.set_int("isPlaying", is_playing ? 1 : 0);
                    pr_notes_direct_program_.set_int("colorMode", color_mode);

                    editor::SharedSelectedNotes* selection = _app->editor_controller.get_selection();
                    const std::uint64_t selection_version = selection ? selection->version() : 0;

                    note_coverage_.begin_frame(tick_pos_offs, zoom_ticks, key_pos, zoom_keys,
                                               window_size_.x - keyboard_height * keyboard_scale_);

                    pr_notes_vao_.bind();
                    pr_notes_ibo_.bind();
                    pr_notes_vbo_.bind();
                    pr_notes_ebo_.bind();

                    glUseProgram(pr_notes_program_.id());

                    _app->note_culler->sync_cull_array_lengths(all_tracks);

                    const std::uint16_t onion_slot_count =
                        static_cast<std::uint16_t>(iter_end - iter_begin);

                    if (std::getenv("ANDROMEDA_ONION_PROBE") != nullptr) {
                        static int reported = 0;
                        if (reported < 3) {
                            reported += 1;
                            std::printf("[onion] state=%d  tracks=%zu  slots=%u  current=%u\n",
                                        static_cast<int>(onion_state), all_tracks.size(),
                                        static_cast<unsigned>(onion_slot_count),
                                        static_cast<unsigned>(nav_curr_track));
                            std::fflush(stdout);
                        }
                    }

                    static const bool no_depth = std::getenv("ANDROMEDA_NO_DEPTH") != nullptr;
                    if (no_depth) {
                        glDisable(GL_DEPTH_TEST);
                        glDepthMask(GL_FALSE);
                    } else {
                        glEnable(GL_DEPTH_TEST);
                        glDepthFunc(GL_LEQUAL);
                        glDepthMask(GL_TRUE);
                    }
                    glClear(GL_DEPTH_BUFFER_BIT);

                    static const bool overdraw_probe =
                        std::getenv("ANDROMEDA_OVERDRAW") != nullptr;
                    if (overdraw_probe) {
                        if (overdraw_query_ == 0) {
                            glGenQueries(1, &overdraw_query_);
                        }
                        glBeginQuery(GL_SAMPLES_PASSED, overdraw_query_);
                    }

                    static const bool phase_probe = std::getenv("ANDROMEDA_TRACK_PHASES") != nullptr;
                    double ph_cull = 0.0;
                    double ph_resident = 0.0;
                    double ph_draw = 0.0;
                    double ph_cpu = 0.0;
                    const auto now = [] { return std::chrono::steady_clock::now(); };
                    const auto since = [](std::chrono::steady_clock::time_point t) {
                        return std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t)
                            .count();
                    };

                    GLuint bound_notes_tex = 0;
                    int last_track_index = -1;
                    std::uint32_t last_onion_meta = 0xFFFFFFFFu;
                    int last_selection_on = -1;
                    int last_indexed = -1;

                    enum class Bound { None, Cpu, Direct };
                    Bound bound = Bound::None;
                    float note_depth = 0.0f;
                    float direct_depth = -1.0f;
                    float cpu_depth = -1.0f;

                    const auto set_note_depth = [&](float depth) { note_depth = depth; };

                    const auto use_direct = [&]() {
                        if (bound != Bound::Direct) {
                            glUseProgram(pr_notes_direct_program_.id());
                            pr_notes_direct_vao_.bind();
                            bound = Bound::Direct;
                        }
                        if (direct_depth != note_depth) {
                            direct_depth = note_depth;
                            pr_notes_direct_program_.set_float("noteDepth", note_depth);
                        }
                    };

                    const auto use_cpu = [&]() {
                        if (bound != Bound::Cpu) {
                            glUseProgram(pr_notes_program_.id());
                            pr_notes_vao_.bind();
                            pr_notes_ibo_.bind();
                            pr_notes_vbo_.bind();
                            pr_notes_ebo_.bind();
                            bound = Bound::Cpu;
                        }
                        if (cpu_depth != note_depth) {
                            cpu_depth = note_depth;
                            pr_notes_program_.set_float("noteDepth", note_depth);
                        }
                    };

                    const auto flush_cpu_notes = [&]() {
                        if (note_id == 0) {
                            return;
                        }
                        use_cpu();
                        pr_notes_ibo_.orphan_and_set(
                            std::span<const RenderPianoRollNote>(notes_render_).first(note_id),
                            notes_render_.size() * sizeof(RenderPianoRollNote));
                        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                                static_cast<GLsizei>(note_id));
                        instances_drawn_ += note_id;
                        cpu_instances_ += note_id;
                        note_id = 0;
                    };

                    const auto bind_cpu_note_state = [&]() { use_cpu(); };

                    constexpr std::size_t PRESSED_SCAN_LIMIT = 1u << 16;
                    const auto mark_pressed_keys = [&](const std::vector<midi::Note>& notes,
                                                       std::size_t first, std::size_t end,
                                                       std::uint16_t color_track) {
                        if (!is_playing) {
                            return;
                        }

                        const std::size_t scan_end = std::min(end, first + PRESSED_SCAN_LIMIT);
                        for (std::size_t i = first; i < scan_end; ++i) {
                            const midi::Note& note = notes[i];
                            if (static_cast<float>(note.get_start()) > playback_pos) {
                                break;
                            }
                            if (static_cast<float>(note.end()) < playback_pos) {
                                continue;
                            }

                            const std::size_t trk_chan =
                                (static_cast<std::size_t>(color_track) << 4) |
                                static_cast<std::size_t>(note.get_channel());
                            // mask required: files may carry key bytes >= 0x80; key_metas_ has 128 entries
                            KeyboardMeta& meta = key_metas_[note.get_key() & 0x7F];
                            meta.pressed = true;
                            meta.color_idx =
                                static_cast<std::uint8_t>(_app->note_colors->get_index(trk_chan));
                        }
                    };

                    const auto draw_note_range =
                        [&](std::size_t cache_key, std::uint16_t color_track,
                            const std::vector<midi::Note>& notes, std::uint64_t revision,
                            std::size_t first, std::size_t end, std::uint32_t onion_meta,
                            const std::vector<std::size_t>* sel_ids,
                            bool coverage_cull) -> bool {
                        if (end <= first) {
                            bail_empty_range_ += 1;
                            return false;
                        }

                        const auto t_ensure = now();
                        NoteGpuCache::Entry* entry =
                            note_gpu_cache_->ensure(cache_key, notes, revision);
                        if (phase_probe) {
                            const double ms = since(t_ensure);
                            ph_draw += ms;
                            static std::size_t stalls = 0;
                            static double stall_ms = 0.0;
                            if (ms > 10.0) {
                                stalls += 1;
                                stall_ms += ms;
                                std::printf("[ensure] stall #%zu: %.2f ms for a %zu-note track"
                                            " (%.0f ms of stalls so far)\n",
                                            stalls, ms, notes.size(), stall_ms);
                                std::fflush(stdout);
                            }
                        }
                        if (entry == nullptr) {
                            bail_no_entry_ += 1;
                            return false;
                        }
                        if (end > entry->note_count) {
                            bail_range_past_end_ += 1;
                            return false;
                        }

                        note_gpu_cache_->ensure_selection(*entry, sel_ids, selection_version);

                        constexpr std::size_t INDEX_WORTH_UPLOADING = 512;

                        bool indexed = false;
                        if (note_coverage_.active() &&
                            ScreenCoverage::worth_running(end - first)) {
                            note_coverage_.select(notes, first, end, coverage_cull);

                            if (note_coverage_.selected().empty()) {
                                mark_pressed_keys(notes, first, end, color_track);
                                return true;
                            }

                            indexed = (end - first) - note_coverage_.selected().size() >=
                                      INDEX_WORTH_UPLOADING;
                        }

                        flush_cpu_notes();

                        use_direct();

                        if (last_track_index != static_cast<int>(color_track)) {
                            last_track_index = static_cast<int>(color_track);
                            pr_notes_direct_program_.set_int("trackIndex", last_track_index);
                        }
                        if (last_onion_meta != onion_meta) {
                            last_onion_meta = onion_meta;
                            pr_notes_direct_program_.set_uint("onionColorMeta", onion_meta);
                        }
                        const int selection_on = entry->selection_any ? 1 : 0;
                        if (last_selection_on != selection_on) {
                            last_selection_on = selection_on;
                            pr_notes_direct_program_.set_int("selectionEnabled", selection_on);
                        }

                        pr_notes_direct_program_.set_uint(
                            "noteBase", static_cast<std::uint32_t>(entry->base_note));

                        if (entry->selection_any) {
                            glActiveTexture(GL_TEXTURE1);
                            glBindTexture(GL_TEXTURE_BUFFER, entry->selection_tex);
                        }

                        const GLuint notes_tex = note_gpu_cache_->texture_of(*entry);
                        if (notes_tex != bound_notes_tex) {
                            glActiveTexture(GL_TEXTURE2);
                            glBindTexture(GL_TEXTURE_BUFFER, notes_tex);
                            bound_notes_tex = notes_tex;
                        }
                        glActiveTexture(GL_TEXTURE0);

                        constexpr std::size_t MAX_NOTES_PER_DRAW = 64u * 1024u * 1024u;

                        std::size_t drawn = 0;
                        if (indexed) {
                            const std::vector<std::uint32_t>& ids = note_coverage_.selected();
                            const std::size_t base = upload_note_indices(ids);
                            if (last_indexed != 1) {
                                last_indexed = 1;
                                pr_notes_direct_program_.set_int("indexed", 1);
                            }

                            for (std::size_t at = 0; at < ids.size(); at += MAX_NOTES_PER_DRAW) {
                                const std::size_t run =
                                    std::min(MAX_NOTES_PER_DRAW, ids.size() - at);
                                pr_notes_direct_program_.set_uint(
                                    "firstNote", static_cast<std::uint32_t>(base + at));
                                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(run * 6));
                            }
                            drawn = ids.size();
                        } else {
                            if (last_indexed != 0) {
                                last_indexed = 0;
                                pr_notes_direct_program_.set_int("indexed", 0);
                            }

                            for (std::size_t at = first; at < end; at += MAX_NOTES_PER_DRAW) {
                                const std::size_t run = std::min(MAX_NOTES_PER_DRAW, end - at);
                                pr_notes_direct_program_.set_uint(
                                    "firstNote", static_cast<std::uint32_t>(at));
                                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(run * 6));
                            }
                            drawn = end - first;
                        }

                        instances_drawn_ += drawn;
                        resident_instances_ += drawn;

                        mark_pressed_keys(notes, first, end, color_track);

                        return true;
                    };

                    const auto culled_range = [&](const NoteOcclusionStore::Culled& culled) {
                        const auto& v = culled.notes;
                        const float left = tick_pos_offs - static_cast<float>(culled.max_length);
                        const auto first = std::partition_point(
                            v.begin(), v.end(),
                            [&](const midi::Note& n) { return static_cast<float>(n.start) < left; });
                        const auto last = std::partition_point(
                            first, v.end(), [&](const midi::Note& n) {
                                return static_cast<float>(n.start) <= tick_pos_offs + zoom_ticks;
                            });
                        return std::pair<std::size_t, std::size_t>{
                            static_cast<std::size_t>(first - v.begin()),
                            static_cast<std::size_t>(last - v.begin())};
                    };

                    const auto draw_track_resident =
                        [&](std::size_t track_index, std::uint16_t color_track,
                            midi::MIDITrack& track_data, std::size_t first, std::size_t end,
                            std::uint32_t onion_meta, const std::vector<std::size_t>* sel_ids,
                            bool allow_cull, bool coverage_cull) -> bool {
                        if (allow_cull) {
                            const NoteOcclusionStore::Culled* culled = note_occlusion_.get(
                                track_index, track_data.revision, track_data.get_notes());

                            if (culled != nullptr) {
                                const auto [cull_first, cull_end] = culled_range(*culled);
                                const std::size_t key = NoteGpuCache::key_for(
                                    track_index, NoteGpuCache::Variant::Culled);
                                if (draw_note_range(key, color_track, culled->notes,
                                                    track_data.revision, cull_first, cull_end,
                                                    onion_meta, nullptr, coverage_cull)) {
                                    return true;
                                }
                            }
                        }

                        return draw_note_range(
                            NoteGpuCache::key_for(track_index, NoteGpuCache::Variant::Exact),
                            color_track, track_data.get_notes(), track_data.revision, first, end,
                            onion_meta, sel_ids, coverage_cull);
                    };

                    set_note_depth(0.02f);
                    if (cur < all_tracks.size()) {
                        const std::vector<midi::Note>& notes = all_tracks[cur].get_notes();
                        if (!notes.empty()) {
                            std::size_t curr_note = 0;

                            _app->note_culler->update_cull_for_track(
                                all_tracks, nav_curr_track, tick_pos_offs, zoom_ticks, false);
                            const auto [note_start, note_end] =
                                _app->note_culler->get_track_cull_range(nav_curr_track);
                            const std::size_t n_off = note_start;
                            std::size_t note_idx = n_off;

                            static const std::vector<std::size_t> EMPTY;
                            editor::SharedSelectedNotes* selection = _app->editor_controller.get_selection();
                            const std::vector<std::size_t>* sel_ptr =
                                selection ? selection->get_selected_ids_in_track(nav_curr_track)
                                          : nullptr;
                            const std::vector<std::size_t>& sel_ids =
                                sel_ptr != nullptr ? *sel_ptr : EMPTY;

                            std::size_t sel_idx = 0;
                            const std::size_t end = std::min(note_end, notes.size());

                            // fixed rust bug: coloured by the leftover loop counter, not this track
                            const bool resident =
                                draw_track_resident(cur, nav_curr_track, all_tracks[cur], n_off,
                                                    end, 0u, sel_ptr, false, false);
                            if (!resident) {
                                note_coverage_.mark_range(notes, n_off, end);
                            }

                            for (std::size_t i = n_off; i < end && !resident; ++i) {
                                const midi::Note& note = notes[i];
                                const std::size_t trk_chan =
                                    (static_cast<std::size_t>(nav_curr_track) << 4) |
                                    static_cast<std::size_t>(note.get_channel());
                                const std::size_t color_index = _app->note_colors->get_index(trk_chan);

                                {
                                    const auto key = static_cast<std::size_t>(note.get_key());
                                    if (note.get_start() <=
                                            util::saturating_cast<editor::MIDITick>(playback_pos) &&
                                        note.end() >=
                                            util::saturating_cast<editor::MIDITick>(playback_pos) &&
                                        is_playing) {
                                        key_metas_[key].pressed = true;
                                        key_metas_[key].color_idx =
                                            static_cast<std::uint8_t>(color_index);
                                    }
                                }

                                if (static_cast<float>(note.get_key()) + 1.0f < key_pos ||
                                    static_cast<float>(note.get_key()) > key_pos + zoom_keys) {
                                    if (sel_idx < sel_ids.size() && note_idx == sel_ids[sel_idx]) {
                                        sel_idx += 1;
                                    }

                                    curr_note += 1;
                                    note_idx += 1;
                                    continue;
                                }

                                {
                                    const float note_bottom =
                                        (static_cast<float>(note.key) - key_pos) / zoom_keys;
                                    const float note_top =
                                        ((static_cast<float>(note.key) + 1.0f) - key_pos) /
                                        zoom_keys;

                                    const float highlight_note_play_size = zoom_ticks * 0.001f;
                                    const bool note_playing =
                                        static_cast<float>(note.get_start()) <
                                            playback_pos + highlight_note_play_size &&
                                        static_cast<float>(note.end()) >
                                            playback_pos - highlight_note_play_size &&
                                        is_playing;

                                    notes_render_[note_id].note_rect = {
                                        (static_cast<float>(note.start) - tick_pos_offs) /
                                            zoom_ticks,
                                        static_cast<float>(note.length) / zoom_ticks, note_bottom,
                                        note_top};

                                    std::uint32_t note_meta =
                                        static_cast<std::uint32_t>(color_index);
                                    note_meta |= static_cast<std::uint32_t>(note.get_velocity())
                                                 << 4;
                                    if (note_playing) {
                                        note_meta |= 1u << 12;
                                    }

                                    if (sel_idx < sel_ids.size() && note_idx == sel_ids[sel_idx]) {
                                        note_meta |= 1u << 13;
                                        sel_idx += 1;
                                    }

                                    notes_render_[note_id].note_meta = note_meta;
                                }

                                note_id += 1;
                                note_idx += 1;

                                if (note_id >= NOTE_BUFFER_SIZE) {
                                    use_cpu();
                                    pr_notes_ibo_.orphan_and_set(
                                        std::span<const RenderPianoRollNote>(notes_render_),
                                        notes_render_.size() * sizeof(RenderPianoRollNote));
                                    glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT,
                                                            nullptr,
                                                            static_cast<GLsizei>(NOTE_BUFFER_SIZE));
                                    instances_drawn_ += NOTE_BUFFER_SIZE;
                                    cpu_instances_ += NOTE_BUFFER_SIZE;
                                    note_id = 0;
                                }

                                curr_note += 1;
                                if (curr_note >= notes.size()) {
                                    break;
                                }
                            }

                            note_coverage_.flush_track();
                        }
                    }

                    for (std::size_t slot = onion_slot_count; slot-- > 0;) {
                        const std::size_t t = iter_begin + slot;
                        // fixed rust bug: used the loop step, so previous/next onion mixed up tracks
                        const auto curr_track = static_cast<std::uint16_t>(t);

                        const std::vector<midi::Note>& notes = all_tracks[t].get_notes();
                        if (notes.empty() || t == cur) {
                            continue;
                        }

                        const auto t_cull = now();
                        _app->note_culler->update_cull_for_track(all_tracks, curr_track,
                                                                 tick_pos_offs, zoom_ticks, false);
                        auto [note_start, note_end] =
                            _app->note_culler->get_track_cull_range(curr_track);
                        std::size_t n_off = note_start;

                        std::size_t curr_note = 0;

                        while (note_end > notes.size()) {
                            _app->note_culler->update_cull_for_track(
                                all_tracks, curr_track, tick_pos_offs, zoom_ticks, true);
                            std::tie(n_off, note_end) =
                                _app->note_culler->get_track_cull_range(curr_track);
                        }

                        if (phase_probe) {
                            ph_cull += since(t_cull);
                        }

                        if (note_end <= n_off) {
                            continue;
                        }

                        set_note_depth(0.05f + 0.9f * (1.0f - static_cast<float>(slot + 1) /
                                                                 static_cast<float>(
                                                                     onion_slot_count + 1)));

                        const auto t_res = now();
                        const bool drew_resident =
                            draw_track_resident(t, curr_track, all_tracks[t], n_off, note_end,
                                                onion_track_color_meta, nullptr, true, true);
                        if (phase_probe) {
                            ph_resident += since(t_res);
                        }
                        if (drew_resident) {
                            resident_tracks_ += 1;
                            note_coverage_.flush_track();
                            continue;
                        }
                        cpu_tracks_ += 1;
                        const auto t_cpu = now();
                        note_coverage_.mark_range(notes, n_off, note_end);

                        for (std::size_t i = n_off; i < note_end; ++i) {
                            const midi::Note& note = notes[i];
                            const std::size_t trk_chan = (static_cast<std::size_t>(curr_track) << 4) |
                                                         static_cast<std::size_t>(note.get_channel());
                            const std::size_t color_index = _app->note_colors->get_index(trk_chan);

                            {
                                const auto key = static_cast<std::size_t>(note.get_key());
                                if (static_cast<float>(note.get_start()) <= playback_pos &&
                                    static_cast<float>(note.end()) >= playback_pos && is_playing) {
                                    key_metas_[key].pressed = true;
                                    key_metas_[key].color_idx =
                                        static_cast<std::uint8_t>(color_index);
                                }
                            }

                            if (static_cast<float>(note.get_key()) + 1.0f < key_pos ||
                                static_cast<float>(note.get_key()) > key_pos + zoom_keys) {
                                curr_note += 1;
                                continue;
                            }

                            {
                                const float note_bottom =
                                    (static_cast<float>(note.get_key()) - key_pos) / zoom_keys;
                                const float note_top =
                                    ((static_cast<float>(note.get_key()) + 1.0f) - key_pos) /
                                    zoom_keys;

                                const float highlight_note_play_size = zoom_ticks * 0.001f;
                                const bool note_playing =
                                    static_cast<float>(note.get_start()) <
                                        playback_pos + highlight_note_play_size &&
                                    static_cast<float>(note.end()) >
                                        playback_pos - highlight_note_play_size &&
                                    is_playing;

                                notes_render_[note_id].note_rect = {
                                    (static_cast<float>(note.start) - tick_pos_offs) / zoom_ticks,
                                    static_cast<float>(note.length) / zoom_ticks, note_bottom,
                                    note_top};

                                std::uint32_t note_meta = static_cast<std::uint32_t>(color_index);
                                note_meta |= static_cast<std::uint32_t>(note.get_velocity()) << 4;
                                if (note_playing) {
                                    note_meta |= 1u << 12;
                                }
                                note_meta |= onion_track_color_meta << 14;
                                notes_render_[note_id].note_meta = note_meta;
                            }

                            note_id += 1;

                            if (note_id >= NOTE_BUFFER_SIZE) {
                                use_cpu();
                                pr_notes_ibo_.orphan_and_set(
                                    std::span<const RenderPianoRollNote>(notes_render_),
                                    notes_render_.size() * sizeof(RenderPianoRollNote));
                                glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                                        static_cast<GLsizei>(NOTE_BUFFER_SIZE));
                                instances_drawn_ += NOTE_BUFFER_SIZE;
                                cpu_instances_ += NOTE_BUFFER_SIZE;
                                note_id = 0;
                            }

                            curr_note += 1;
                            if (curr_note >= notes.size()) {
                                break;
                            }
                        }

                        if (phase_probe) {
                            ph_cpu += since(t_cpu);
                        }

                        note_coverage_.flush_track();
                    }

                    if (phase_probe) {
                        static double worst = 0.0;
                        const double total = ph_cull + ph_resident + ph_draw + ph_cpu;
                        if (total > worst) {
                            worst = total;
                            std::printf("[phases] %.1f ms | cull %.1f  resident %.1f (upload %.1f)"
                                        "  cpu-pack %.1f | %u tracks, %zu instances\n",
                                        total - ph_draw, ph_cull, ph_resident, ph_draw, ph_cpu,
                                        static_cast<unsigned>(onion_slot_count), instances_drawn_);
                            std::fflush(stdout);
                        }
                    }

                    flush_cpu_notes();
                    if (note_id != 0) {
                        pr_notes_ibo_.orphan_and_set(
                            std::span<const RenderPianoRollNote>(notes_render_).first(note_id),
                            notes_render_.size() * sizeof(RenderPianoRollNote));
                        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                                static_cast<GLsizei>(note_id));
                        instances_drawn_ += note_id;
                        cpu_instances_ += note_id;
                    }

                    if (overdraw_probe) {
                        glEndQuery(GL_SAMPLES_PASSED);
                        GLuint64 samples = 0;
                        glGetQueryObjectui64v(overdraw_query_, GL_QUERY_RESULT, &samples);
                        const double px = static_cast<double>(window_size_.x) *
                                          static_cast<double>(window_size_.y);
                        if (samples > overdraw_peak_) {
                            overdraw_peak_ = samples;
                            std::printf("[overdraw] %llu fragments over %.0f pixels = %.1fx, "
                                        "%zu instances\n",
                                        static_cast<unsigned long long>(samples), px,
                                        px > 0.0 ? static_cast<double>(samples) / px : 0.0,
                                        instances_drawn_);
                            std::fflush(stdout);
                        }
                    }
                }
            }

            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);

            if (ghost_notes != nullptr) {
                std::size_t note_id = 0;
                const std::vector<midi::Note>& notes = *ghost_notes;

                glUseProgram(pr_notes_program_.id());
                pr_notes_vao_.bind();
                pr_notes_ibo_.bind();
                pr_notes_vbo_.bind();
                pr_notes_ebo_.bind();

                for (const midi::Note& note : notes) {
                    const float note_bottom =
                        (static_cast<float>(note.key) - key_pos) / zoom_keys;
                    const float note_top =
                        ((static_cast<float>(note.key) + 1.0f) - key_pos) / zoom_keys;

                    const std::size_t trk_chan = (static_cast<std::size_t>(nav_curr_track) << 4) |
                                                 static_cast<std::size_t>(note.get_channel());

                    notes_render_[note_id].note_rect = {
                        (static_cast<float>(note.start) - tick_pos) / zoom_ticks,
                        static_cast<float>(note.length) / zoom_ticks, note_bottom, note_top};

                    std::uint32_t note_meta =
                        static_cast<std::uint32_t>(_app->note_colors->get_index(trk_chan));
                    note_meta |= static_cast<std::uint32_t>(note.get_velocity()) << 4;
                    notes_render_[note_id].note_meta = note_meta;

                    note_id += 1;
                    if (note_id >= NOTE_BUFFER_SIZE) {
                        pr_notes_ibo_.orphan_and_set(
                            std::span<const RenderPianoRollNote>(notes_render_),
                            notes_render_.size() * sizeof(RenderPianoRollNote));
                        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                                static_cast<GLsizei>(NOTE_BUFFER_SIZE));
                        note_id = 0;
                    }
                }

                if (note_id != 0) {
                    pr_notes_ibo_.orphan_and_set(
                        std::span<const RenderPianoRollNote>(notes_render_).first(note_id),
                        notes_render_.size() * sizeof(RenderPianoRollNote));
                    glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                            static_cast<GLsizei>(note_id));
                }
            }
        }

        glUseProgram(0);
    }

    {
        glUseProgram(pr_keyboard_program_.id());

        glActiveTexture(GL_TEXTURE0);
        _app->note_colors->get_texture().bind();

        pr_keyboard_program_.set_int("noteColorTexture", 0);
        pr_keyboard_program_.set_float("width", window_size_.x);
        pr_keyboard_program_.set_float("height", window_size_.y);
        pr_keyboard_program_.set_float("keyboardHeight", keyboard_height * keyboard_scale_);

        {
            const float key_start = key_pos;
            const float key_end = key_pos + zoom_keys;

            pr_keyboard_program_.set_float("prBarBottom", -key_start / (key_end - key_start));
            pr_keyboard_program_.set_float("prBarTop",
                                           (128.0f - key_start) / (key_end - key_start));

            pr_keyboard_vertex_array_.bind();
            pr_keyboard_instance_buffer_.bind();
            pr_keyboard_vertex_buffer_.bind();
            pr_keyboard_index_buffer_.bind();

            for (std::size_t i = 0; i < key_ids_.size(); ++i) {
                kb_render_[i].meta0 = key_metas_[key_ids_[i]].get_meta();
            }

            pr_keyboard_instance_buffer_.set_data(
                std::span<const RenderPianoRollKeyboard>(kb_render_), GL_DYNAMIC_DRAW);
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, 128);
        }

        for (KeyboardMeta& key_meta : key_metas_) {
            key_meta.pressed = false;
        }
    }
}

}
