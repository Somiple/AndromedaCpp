#include "app/rendering/data_view.h"

#include <algorithm>
#include <bit>
#include <mutex>
#include <shared_mutex>
#include <format>
#include <cstdlib>
#include <vector>

#include "util/debugger.h"

#include "app/rendering/piano_roll.h"
#include "editor/util.h"

namespace andromeda::app::rendering {

using midi::Note;
using util::Debugger;

DataViewRenderer::DataViewRenderer(
    const util::SharedPtr<editor::ProjectManager>& project_manager,
    util::SharedMutPtr<ViewSettings> view_settings,
    util::SharedPtr<editor::PianoRollNavigation> nav,
    std::shared_ptr<audio::AudioEngine> playback_manager,
    std::shared_ptr<editor::BarCacher> bar_cacher_,
    std::shared_ptr<NoteColors> note_colors,
    std::shared_ptr<NoteCullHelper> note_cull_helper,
    std::shared_ptr<editor::SharedSelectedNotes> shared_selected_notes)
    : navigation(std::move(nav)),
      bar_cacher(std::move(bar_cacher_)),
      view_settings_(std::move(view_settings)),
      playback_manager_(std::move(playback_manager)),
      note_colors_(std::move(note_colors)),
      note_cull_helper_(std::move(note_cull_helper)),
      selected_(std::move(shared_selected_notes)) {

    dv_program_ = ShaderProgram::create_from_files("./assets/shaders/data_view_bg");
    dv_handles_program_ = ShaderProgram::create_from_files("./assets/shaders/data_view_handles");
    dv_handles_direct_program_ =
        ShaderProgram::create_from_files("./assets/shaders/data_view_handle_direct");
    dv_cache_program_ = ShaderProgram::create_from_files("./assets/shaders/data_view_cache");

    dv_direct_vao_ = VertexArray::create();

    dv_vertex_buffer_ = Buffer(GL_ARRAY_BUFFER);
    dv_vertex_buffer_.set_data(std::span<const Vertex>(QUAD_VERTICES), GL_STATIC_DRAW);

    dv_index_buffer_ = Buffer(GL_ELEMENT_ARRAY_BUFFER);
    dv_index_buffer_.set_data(std::span<const std::uint32_t>(DV_QUAD_INDICES), GL_STATIC_DRAW);

    dv_vertex_array_ = VertexArray::create();
    const GLint pos_attrib = dv_program_.get_attrib_location("vPos");
    SET_ATTRIBUTE(GL_FLOAT, dv_vertex_array_, pos_attrib, Vertex, position);

    dv_instance_buffer_ = Buffer(GL_ARRAY_BUFFER);
    bars_render_.assign(DV_BAR_BUFFER_SIZE, RenderDataViewBar{0.0f, 1.0f, 0});
    dv_instance_buffer_.set_data(std::span<const RenderDataViewBar>(bars_render_),
                                 GL_DYNAMIC_DRAW);

    const GLint dv_bar_start = dv_program_.get_attrib_location("barStart");
    SET_ATTRIBUTE(GL_FLOAT, dv_vertex_array_, dv_bar_start, RenderDataViewBar, bar_start);
    const GLint dv_bar_length = dv_program_.get_attrib_location("barLength");
    SET_ATTRIBUTE(GL_FLOAT, dv_vertex_array_, dv_bar_length, RenderDataViewBar, bar_length);
    const GLint dv_bar_number = dv_program_.get_attrib_location("barNumber");
    SET_ATTRIBUTE(GL_UNSIGNED_INT, dv_vertex_array_, dv_bar_number, RenderDataViewBar, bar_number);

    glVertexAttribDivisor(1, 1);
    glVertexAttribDivisor(2, 1);
    glVertexAttribDivisor(3, 1);

    dv_handles_vbo_ = Buffer(GL_ARRAY_BUFFER);
    dv_handles_vbo_.set_data(std::span<const Vertex>(QUAD_VERTICES), GL_STATIC_DRAW);

    dv_handles_ebo_ = Buffer(GL_ELEMENT_ARRAY_BUFFER);
    dv_handles_ebo_.set_data(std::span<const std::uint32_t>(DV_QUAD_INDICES), GL_STATIC_DRAW);

    dv_handles_vao_ = VertexArray::create();
    SET_ATTRIBUTE(GL_FLOAT, dv_handles_vao_, 0, Vertex, position);

    dv_handles_ibo_ = Buffer(GL_ARRAY_BUFFER);
    dv_handles_render_.assign(HANDLE_BUFFER_SIZE,
                              RenderDataViewHandle{{0.0f, 1.0f, 0.5f, 0.5f}, 0});
    dv_handles_ibo_.set_data(std::span<const RenderDataViewHandle>(dv_handles_render_),
                             GL_DYNAMIC_DRAW);

    const GLint dv_handles_rect = dv_handles_program_.get_attrib_location("handleRect");
    SET_ATTRIBUTE(GL_FLOAT, dv_handles_vao_, dv_handles_rect, RenderDataViewHandle, handle_rect);
    const GLint dv_handles_meta = dv_handles_program_.get_attrib_location("handleMeta");
    SET_ATTRIBUTE(GL_UNSIGNED_INT, dv_handles_vao_, dv_handles_meta, RenderDataViewHandle,
                  handle_meta);

    glVertexAttribDivisor(1, 1);
    glVertexAttribDivisor(2, 1);

    {
        std::shared_lock lock(project_manager->mutex);
        all_tracks_ = project_manager->value.get_tracks();
    }
}

DataViewRenderer::~DataViewRenderer() {
    if (handle_cache_.fbo != 0) {
        glDeleteFramebuffers(1, &handle_cache_.fbo);
    }
    if (handle_cache_.tex != 0) {
        glDeleteTextures(1, &handle_cache_.tex);
    }
}

std::uint64_t DataViewRenderer::handles_key(const std::vector<midi::MIDITrack>& tracks,
                                            const OnionRange& onion, std::uint16_t nav_curr_track,
                                            float tick_pos_offs, float zoom_ticks,
                                            const GLint viewport[4]) const {
    std::uint64_t key = 0x9E3779B97F4A7C15ull;
    const auto mix = [&key](std::uint64_t v) {
        key ^= v + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2);
    };

    mix(std::bit_cast<std::uint32_t>(tick_pos_offs));
    mix(std::bit_cast<std::uint32_t>(zoom_ticks));
    mix(static_cast<std::uint64_t>(viewport[2]));
    mix(static_cast<std::uint64_t>(viewport[3]));
    mix(std::bit_cast<std::uint32_t>(window_size_.x));
    mix(std::bit_cast<std::uint32_t>(window_size_.y));

    mix(nav_curr_track);
    mix(onion.begin);
    mix(onion.end);
    mix(onion.color_meta);
    mix(static_cast<std::uint64_t>(note_colors_->get_index_type()));
    mix(note_colors_->version());
    mix(selected_ ? selected_->version() : 0);

    mix(tracks.size());
    for (const midi::MIDITrack& track : tracks) {
        mix(track.revision);
        mix(track.get_notes().size());
    }

    return key != 0 ? key : 1;
}

bool DataViewRenderer::prepare_handle_cache(int width, int height) {
    static const bool disabled = std::getenv("ANDROMEDA_DV_NO_CACHE") != nullptr;

    HandleCache& cache = handle_cache_;
    if (disabled || cache.unavailable || width <= 0 || height <= 0) {
        return false;
    }
    if (cache.fbo != 0 && cache.width == width && cache.height == height) {
        return true;
    }

    if (cache.tex == 0) {
        glGenTextures(1, &cache.tex);
    }
    // fixed: unbinding here left the handles without their colour table (grey strip)
    GLint previous_tex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_tex);
    glBindTexture(GL_TEXTURE_2D, cache.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_tex));

    if (cache.fbo == 0) {
        glGenFramebuffers(1, &cache.fbo);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, cache.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cache.tex, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!complete) {
        Debugger::log_warning("Velocity strip cache unavailable; drawing the strip every frame.");
        cache.unavailable = true;
        return false;
    }

    cache.width = width;
    cache.height = height;
    cache.valid = false;
    return true;
}

void DataViewRenderer::composite_handle_cache() {
    glUseProgram(dv_cache_program_.id());
    dv_cache_program_.set_int("handles", 0);
    dv_direct_vao_.bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, handle_cache_.tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    note_colors_->get_texture().bind();
}

float DataViewRenderer::get_time() const {
    std::shared_lock nav_lock(navigation->mutex);
    std::lock_guard vs_lock(view_settings_->mutex);

    if (view_settings_->value.pr_autoscroll) {
        if (playback_manager_->is_playing()) {
            return static_cast<float>(playback_manager_->get_playback_ticks());
        }
        return navigation->value.tick_pos_smoothed;
    }

    return navigation->value.tick_pos_smoothed;
}

DataViewRenderer::OnionRange DataViewRenderer::onion_range(std::size_t track_count,
                                                           std::uint16_t nav_curr_track) const {
    VS_PianoRoll_OnionState onion_state{};
    VS_PianoRoll_OnionColoring onion_coloring{};
    {
        std::lock_guard lock(view_settings_->mutex);
        onion_state = view_settings_->value.pr_onion_state;
        onion_coloring = view_settings_->value.pr_onion_coloring;
    }

    const auto cur = static_cast<std::size_t>(nav_curr_track);
    OnionRange r{cur, cur, 0};

    switch (onion_state) {
    case VS_PianoRoll_OnionState::NoOnion:
        break;
    case VS_PianoRoll_OnionState::ViewAll:
        r.begin = 0;
        r.end = track_count;
        break;
    case VS_PianoRoll_OnionState::ViewPrevious:
        if (cur != 0) {
            r.begin = cur - 1;
            r.end = cur + 1;
        }
        break;
    case VS_PianoRoll_OnionState::ViewNext:
        if (cur != track_count - 1) {
            r.begin = cur;
            r.end = cur + 2;
        }
        break;
    }

    r.begin = std::min(r.begin, track_count);
    r.end = std::min(r.end, track_count);

    switch (onion_coloring) {
    case VS_PianoRoll_OnionColoring::FullColor:    r.color_meta = 0b00; break;
    case VS_PianoRoll_OnionColoring::PartialColor: r.color_meta = 0b01; break;
    case VS_PianoRoll_OnionColoring::GrayedOut:    r.color_meta = 0b10; break;
    }

    return r;
}

void DataViewRenderer::draw() {
    {
        std::lock_guard lock(view_settings_->mutex);
        if (view_settings_->value.pr_dataview_state == VS_PianoRoll_DataViewState::Hidden) {
            return;
        }
    }

    const float tick_pos = get_time();

    float zoom_ticks = 0.0f;
    {
        std::shared_lock lock(navigation->mutex);
        zoom_ticks = navigation->value.zoom_ticks_smoothed;
    }

    {
        bool autoscroll = false;
        {
            std::lock_guard lock(view_settings_->mutex);
            autoscroll = view_settings_->value.pr_autoscroll;
        }

        // fixed rust bug: taken once at play start and scaled on zoom, so it drifted from the
        // playhead line; it is now the same live formula as the piano roll
        view_offset_ = 0.0f;
        if (playback_manager_->is_playing() && autoscroll) {
            std::shared_lock lock(navigation->mutex);
            view_offset_ = navigation->value.tick_pos_smoothed -
                           static_cast<float>(playback_manager_->get_playback_start_tick());
        }
        // the view on screen stops at the song start, as the playhead line's does
        view_offset_ = std::max(view_offset_, -tick_pos);
    }

    const float tick_pos_offs = tick_pos + view_offset_;

    {
        glUseProgram(dv_program_.id());

        dv_program_.set_float("width", window_size_.x);
        dv_program_.set_float("height", window_size_.y);
        dv_program_.set_float("ppqNorm", static_cast<float>(ppq) / zoom_ticks);

        float curr_bar_tick = 0.0f;
        std::size_t bar_num = 0;
        std::size_t bar_id = 0;

        const auto flush_bars = [&](std::size_t count) {
            dv_vertex_array_.bind();
            dv_instance_buffer_.bind();
            dv_vertex_buffer_.bind();
            dv_index_buffer_.bind();
            dv_instance_buffer_.set_data(std::span<const RenderDataViewBar>(bars_render_),
                                         GL_DYNAMIC_DRAW);
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                    static_cast<GLsizei>(count));
        };

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
                RenderDataViewBar{(curr_bar_tick - tick_pos_offs) / zoom_ticks,
                                  static_cast<float>(bar_length) / zoom_ticks,
                                  static_cast<std::uint32_t>(bar_num)};
            bar_id += 1;
            if (bar_id >= 32) {
                flush_bars(32);
                bar_id = 0;
            }

            curr_bar_tick += static_cast<float>(bar_length);
            bar_num += 1;
        }

        if (bar_id != 0) {
            flush_bars(bar_id);
        }

        glUseProgram(0);
    }

    {
        glUseProgram(dv_handles_program_.id());

        glActiveTexture(GL_TEXTURE0);
        note_colors_->get_texture().bind();

        dv_handles_program_.set_int("noteColorTexture", 0);
        dv_handles_program_.set_float("width", window_size_.x);
        dv_handles_program_.set_float("height", window_size_.y);

        VS_PianoRoll_DataViewState curr_data_view{};
        {
            std::lock_guard lock(view_settings_->mutex);
            curr_data_view = view_settings_->value.pr_dataview_state;
        }

        switch (curr_data_view) {
        case VS_PianoRoll_DataViewState::NoteVelocities:
            draw_note_velocities(tick_pos, zoom_ticks);
            break;
        case VS_PianoRoll_DataViewState::PitchBend:
            draw_channel_event_data(tick_pos, zoom_ticks, midi::PitchBend{0, 0});
            break;
        default:
            break;
        }

        glUseProgram(0);
    }
}

void DataViewRenderer::set_direct_frame_uniforms(float tick_pos, float zoom_ticks) {
    int color_mode = 0;
    switch (note_colors_->get_index_type()) {
    case NoteColorIndexing::Channel:      color_mode = 0; break;
    case NoteColorIndexing::Track:        color_mode = 1; break;
    case NoteColorIndexing::ChannelTrack: color_mode = 2; break;
    }

    const bool is_playing = playback_manager_->is_playing();
    const float playback_pos = static_cast<float>(playback_manager_->get_playback_ticks());

    glUseProgram(dv_handles_direct_program_.id());
    dv_handles_direct_program_.set_int("noteColorTexture", 0);
    dv_handles_direct_program_.set_int("selectedBits", 1);
    dv_handles_direct_program_.set_int("noteData", 2);
    dv_handles_direct_program_.set_float("width", window_size_.x);
    dv_handles_direct_program_.set_float("height", window_size_.y);
    dv_handles_direct_program_.set_float("tickPos", tick_pos);
    dv_handles_direct_program_.set_float("zoomTicks", zoom_ticks);
    dv_handles_direct_program_.set_int("colorMode", color_mode);
    dv_handles_direct_program_.set_float("playbackPos", playback_pos);
    dv_handles_direct_program_.set_float("highlightSize", zoom_ticks * 0.001f);
    dv_handles_direct_program_.set_int("isPlaying", is_playing ? 1 : 0);

    dv_direct_vao_.bind();
    direct_state_ = DirectState{};
    direct_state_.color_mode = color_mode;
}

bool DataViewRenderer::draw_track_direct(const VisibleTrack& visible,
                                         const std::vector<Note>& notes, std::uint64_t revision,
                                         std::uint32_t onion_meta, bool is_current_track) {
    const std::size_t track = visible.track;
    const std::size_t first = visible.first;
    const std::size_t end = visible.end;
    const std::size_t stride = visible.stride;

    if (!note_gpu_cache_ || !NoteGpuCache::enabled() || end <= first) {
        return end <= first;
    }

    const std::size_t cache_key = NoteGpuCache::key_for(track, NoteGpuCache::Variant::Exact);
    NoteGpuCache::Entry* entry = note_gpu_cache_->ensure(cache_key, notes, revision);
    if (entry == nullptr || end > entry->note_count) {
        return false;
    }

    const std::vector<std::size_t>* sel_ids =
        (is_current_track && selected_) ? selected_->get_selected_ids_in_track(
                                              static_cast<std::uint16_t>(track))
                                        : nullptr;
    const std::uint64_t selection_version = selected_ ? selected_->version() : 0;
    note_gpu_cache_->ensure_selection(*entry, sel_ids, selection_version);

    DirectState& state = direct_state_;

    if (state.color_mode != 0 && state.track != static_cast<int>(track)) {
        state.track = static_cast<int>(track);
        dv_handles_direct_program_.set_int("trackIndex", state.track);
    }
    if (state.onion_meta != onion_meta) {
        state.onion_meta = onion_meta;
        dv_handles_direct_program_.set_uint("onionColorMeta", onion_meta);
    }
    const int selection_on = entry->selection_any ? 1 : 0;
    if (state.selection_on != selection_on) {
        state.selection_on = selection_on;
        dv_handles_direct_program_.set_int("selectionEnabled", selection_on);
    }
    if (state.stride != stride) {
        state.stride = stride;
        dv_handles_direct_program_.set_uint("noteStride", static_cast<std::uint32_t>(stride));
    }
    dv_handles_direct_program_.set_uint("noteBase", static_cast<std::uint32_t>(entry->base_note));

    if (entry->selection_any) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_BUFFER, entry->selection_tex);
        glActiveTexture(GL_TEXTURE0);
    }
    const GLuint notes_tex = note_gpu_cache_->texture_of(*entry);
    if (state.notes_tex != notes_tex) {
        state.notes_tex = notes_tex;
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_BUFFER, notes_tex);
        glActiveTexture(GL_TEXTURE0);
    }

    constexpr std::size_t MAX_NOTES_PER_DRAW = 64u * 1024u * 1024u;
    constexpr std::size_t VERTICES_PER_HANDLE = 12;
    const std::size_t handles = (end - first + stride - 1) / stride;
    for (std::size_t done = 0; done < handles; done += MAX_NOTES_PER_DRAW) {
        const std::size_t run = std::min(MAX_NOTES_PER_DRAW, handles - done);
        dv_handles_direct_program_.set_uint("firstNote",
                                            static_cast<std::uint32_t>(first + done * stride));
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(run * VERTICES_PER_HANDLE));
    }

    return true;
}

std::size_t DataViewRenderer::max_handles_per_pixel() {
    static const std::size_t per_pixel = [] {
        if (const char* env = std::getenv("ANDROMEDA_DV_MAX_DENSITY");
            env != nullptr && env[0] != 0) {
            return static_cast<std::size_t>(std::strtoull(env, nullptr, 10));
        }
        return DEFAULT_MAX_HANDLES_PER_PIXEL;
    }();
    return per_pixel;
}

void DataViewRenderer::apply_density_cap(std::vector<VisibleTrack>& visible) const {
    std::size_t total = 0;
    std::size_t largest = 0;
    for (VisibleTrack& v : visible) {
        v.stride = 1;
        total += v.end - v.first;
        largest = std::max(largest, v.end - v.first);
    }

    const std::size_t per_pixel = max_handles_per_pixel();
    if (per_pixel == 0) {
        return;
    }
    const auto columns = std::max<std::size_t>(1, static_cast<std::size_t>(window_size_.x));
    const std::size_t budget = columns * per_pixel;
    if (total <= budget) {
        return;
    }

    const auto total_at = [&](std::size_t c) {
        std::size_t sum = 0;
        for (const VisibleTrack& v : visible) {
            sum += std::min(v.end - v.first, c);
        }
        return sum;
    };

    std::size_t lo = 1;
    std::size_t hi = largest;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo + 1) / 2;
        if (total_at(mid) <= budget) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }

    for (VisibleTrack& v : visible) {
        const std::size_t n = v.end - v.first;
        if (n > lo) {
            v.stride = (n + lo - 1) / lo;
        }
    }
}

void DataViewRenderer::draw_note_velocities(float tick_pos, float zoom_ticks) {
    std::uint16_t nav_curr_track = 0;
    {
        std::shared_lock lock(navigation->mutex);
        nav_curr_track = navigation->value.curr_track;
    }

    std::shared_lock tracks_lock(all_tracks_->mutex);
    const std::vector<midi::MIDITrack>& tracks = all_tracks_->value;
    if (tracks.empty()) {
        return;
    }

    const OnionRange onion = onion_range(tracks.size(), nav_curr_track);
    const float tick_pos_offs = tick_pos + view_offset_;

    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);

    const std::uint64_t key =
        playback_manager_->is_playing()
            ? 0
            : handles_key(tracks, onion, nav_curr_track, tick_pos_offs, zoom_ticks, viewport);
    const bool still = key != 0 && key == last_handles_key_;
    last_handles_key_ = key;

    std::vector<VisibleTrack> onion_tracks;
    std::vector<VisibleTrack> cpu_tracks;
    bool current_on_cpu = false;

    const HandleCache& cache = handle_cache_;
    if (still && cache.valid && cache.key == key && cache.width == viewport[2] &&
        cache.height == viewport[3]) {
        composite_handle_cache();
    } else {
        note_cull_helper_->sync_cull_array_lengths(tracks);

        onion_tracks.reserve(onion.end > onion.begin ? onion.end - onion.begin : 0);

        for (std::size_t t = onion.begin; t < onion.end; ++t) {
            // fixed rust bug: counted loop steps from 0, so previous/next onion mixed up tracks
            const auto curr_track = static_cast<std::uint16_t>(t);
            const std::vector<Note>& notes = tracks[t].get_notes();
            if (notes.empty() || curr_track == nav_curr_track) {
                continue;
            }

            note_cull_helper_->update_cull_for_track(tracks, curr_track, tick_pos_offs,
                                                     zoom_ticks, false);
            auto [note_start, note_end] = note_cull_helper_->get_track_cull_range(curr_track);
            if (note_end > notes.size()) {
                note_cull_helper_->update_cull_for_track(tracks, curr_track, tick_pos_offs,
                                                         zoom_ticks, true);
                std::tie(note_start, note_end) =
                    note_cull_helper_->get_track_cull_range(curr_track);
            }

            const std::size_t end = std::min(note_end, notes.size());
            if (end > note_start) {
                onion_tracks.push_back(VisibleTrack{curr_track, note_start, end});
            }
        }

        bool has_current = false;
        if (static_cast<std::size_t>(nav_curr_track) < tracks.size() &&
            !tracks[nav_curr_track].get_notes().empty()) {
            note_cull_helper_->update_cull_for_track(tracks, nav_curr_track, tick_pos_offs,
                                                     zoom_ticks, false);
            const auto [note_start, note_end] =
                note_cull_helper_->get_track_cull_range(nav_curr_track);
            const std::size_t end = std::min(note_end, tracks[nav_curr_track].get_notes().size());
            if (end > note_start) {
                onion_tracks.push_back(VisibleTrack{nav_curr_track, note_start, end});
                has_current = true;
            }
        }

        apply_density_cap(onion_tracks);

        const bool fill_cache = still && prepare_handle_cache(viewport[2], viewport[3]);
        GLint scissor_box[4] = {};
        GLboolean scissor_on = GL_FALSE;
        GLfloat clear_color[4] = {};
        if (fill_cache) {
            glGetIntegerv(GL_SCISSOR_BOX, scissor_box);
            scissor_on = glIsEnabled(GL_SCISSOR_TEST);
            glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);

            glBindFramebuffer(GL_FRAMEBUFFER, handle_cache_.fbo);
            glViewport(0, 0, viewport[2], viewport[3]);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        set_direct_frame_uniforms(tick_pos_offs, zoom_ticks);

        glDisable(GL_BLEND);

        for (std::size_t i = 0; i < onion_tracks.size(); ++i) {
            const VisibleTrack& dt = onion_tracks[i];
            const bool is_current = has_current && i + 1 == onion_tracks.size();

            if (draw_track_direct(dt, tracks[dt.track].get_notes(), tracks[dt.track].revision,
                                  is_current ? 0u : onion.color_meta, is_current)) {
                continue;
            }
            if (is_current) {
                current_on_cpu = true;
            } else {
                cpu_tracks.push_back(dt);
            }
        }

        glEnable(GL_BLEND);

        if (fill_cache) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glScissor(scissor_box[0], scissor_box[1], scissor_box[2], scissor_box[3]);
            if (scissor_on != GL_FALSE) {
                glEnable(GL_SCISSOR_TEST);
            }
            glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);

            handle_cache_.key = key;
            handle_cache_.valid = cpu_tracks.empty() && !current_on_cpu;
            composite_handle_cache();
        }
    }

    const bool ghosts = ghost_notes != nullptr;
    if (cpu_tracks.empty() && !current_on_cpu && !ghosts) {
        return;
    }

    dv_handles_vao_.bind();
    dv_handles_ibo_.bind();
    dv_handles_vbo_.bind();
    dv_handles_ebo_.bind();
    glUseProgram(dv_handles_program_.id());

    std::size_t handle_id = 0;

    const auto flush = [&](std::size_t count) {
        dv_handles_ibo_.set_sub_data(
            0, std::span<const RenderDataViewHandle>(dv_handles_render_).first(count));
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                static_cast<GLsizei>(count));
    };

    const auto push_handle = [&](const Note& note, std::size_t track, std::uint32_t extra_meta) {
        const std::size_t trk_chan =
            (track << 4) | static_cast<std::size_t>(note.get_channel());

        std::uint32_t note_meta = static_cast<std::uint32_t>(note_colors_->get_index(trk_chan));
        note_meta |= static_cast<std::uint32_t>(note.get_velocity()) << 4;
        note_meta |= extra_meta;

        dv_handles_render_[handle_id] = RenderDataViewHandle{
            {(static_cast<float>(note.start) - tick_pos_offs) / zoom_ticks,
             static_cast<float>(note.length) / zoom_ticks, 0.0f,
             static_cast<float>(note.get_velocity()) / 127.0f},
            note_meta};

        handle_id += 1;
        if (handle_id >= HANDLE_BUFFER_SIZE) {
            flush(HANDLE_BUFFER_SIZE);
            handle_id = 0;
        }
    };

    for (const VisibleTrack& dt : cpu_tracks) {
        const std::vector<Note>& notes = tracks[dt.track].get_notes();
        for (std::size_t i = dt.first; i < dt.end; i += dt.stride) {
            push_handle(notes[i], dt.track, onion.color_meta << 14);
        }
    }

    if (current_on_cpu) {
        const VisibleTrack& current = onion_tracks.back();
        const std::vector<Note>& notes = tracks[nav_curr_track].get_notes();

        static const std::vector<std::size_t> EMPTY;
        const std::vector<std::size_t>* sel_ptr =
            selected_ ? selected_->get_selected_ids_in_track(nav_curr_track) : nullptr;
        const std::vector<std::size_t>& sel_ids = sel_ptr != nullptr ? *sel_ptr : EMPTY;

        std::size_t sel_idx = 0;
        for (std::size_t i = current.first; i < current.end; i += current.stride) {
            while (sel_idx < sel_ids.size() && sel_ids[sel_idx] < i) {
                sel_idx += 1;
            }
            std::uint32_t extra = 0;
            if (sel_idx < sel_ids.size() && i == sel_ids[sel_idx]) {
                extra |= 1u << 13;
                sel_idx += 1;
            }
            push_handle(notes[i], nav_curr_track, extra);
        }
    }

    if (ghosts) {
        std::lock_guard lock(ghost_notes->mutex);
        for (const Note& note : ghost_notes->value) {
            push_handle(note, nav_curr_track, 0);
        }
    }

    if (handle_id != 0) {
        flush(handle_id);
    }
}

void DataViewRenderer::draw_channel_event_data(
    float tick_pos, float zoom_ticks, const midi::ChannelEventType& channel_event_type) {
    std::uint16_t nav_curr_track = 0;
    {
        std::shared_lock lock(navigation->mutex);
        nav_curr_track = navigation->value.curr_track;
    }

    std::shared_lock tracks_lock(all_tracks_->mutex);
    const std::vector<midi::MIDITrack>& tracks = all_tracks_->value;
    if (tracks.empty()) {
        return;
    }

    const OnionRange onion = onion_range(tracks.size(), nav_curr_track);

    std::size_t handle_id = 0;

    const float tick_pos_offs = tick_pos + view_offset_;

    dv_handles_vao_.bind();
    dv_handles_ibo_.bind();
    dv_handles_vbo_.bind();
    dv_handles_ebo_.bind();

    const auto flush = [&](std::size_t count) {
        dv_handles_ibo_.set_sub_data(
            0, std::span<const RenderDataViewHandle>(dv_handles_render_).first(count));
        glUseProgram(dv_handles_program_.id());
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr,
                                static_cast<GLsizei>(count));
    };

    const auto emit = [&](const midi::ChannelEvent& curr_ev, const midi::ChannelEvent& next_ev,
                          std::uint16_t track_for_color, std::uint32_t onion_meta) {
        const std::size_t trk_chan = (static_cast<std::size_t>(track_for_color) << 4) |
                                     static_cast<std::size_t>(curr_ev.channel);

        const editor::MIDITick evt_duration = next_ev.tick - curr_ev.tick;

        float value = 0.5f;
        float default_val = 0.0f;
        if (const auto* pb = std::get_if<midi::PitchBend>(&curr_ev.event_type)) {
            const auto raw = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(pb->msb) << 7) | static_cast<std::uint16_t>(pb->lsb));
            const float value_norm = static_cast<float>(static_cast<std::int16_t>(raw) - 8192) /
                                     8192.0f;
            value = value_norm * 0.5f + 0.5f;
            default_val = 0.5f;
        }

        dv_handles_render_[handle_id] = RenderDataViewHandle{
            {(static_cast<float>(curr_ev.tick) - tick_pos_offs) / zoom_ticks,
             static_cast<float>(evt_duration) / zoom_ticks, default_val, value},
            static_cast<std::uint32_t>(note_colors_->get_index(trk_chan)) |
                (onion_meta << 14) | (127u << 4)};

        handle_id += 1;

        if (handle_id >= HANDLE_BUFFER_SIZE) {
            flush(HANDLE_BUFFER_SIZE);
            handle_id = 0;
        }
    };

    for (std::size_t t = onion.begin; t < onion.end; ++t) {
        // fixed rust bug: counted loop steps from 0, so previous/next onion mixed up tracks
        const auto curr_track = static_cast<std::uint16_t>(t);
        const std::vector<midi::ChannelEvent>& ch_evs = tracks[t].get_channel_evs();
        if (ch_evs.empty() || curr_track == nav_curr_track) {
            continue;
        }

        auto curr_ch_event =
            editor::get_next_specific_ch_ev_idx(ch_evs, channel_event_type, std::nullopt);
        if (!curr_ch_event.has_value()) {
            continue;
        }

        while (const auto next_ch_event = editor::get_next_specific_ch_ev_idx(
                   ch_evs, channel_event_type, *curr_ch_event + 1)) {
            emit(ch_evs[*curr_ch_event], ch_evs[*next_ch_event], curr_track, onion.color_meta);
            curr_ch_event = next_ch_event;
        }
    }

    if (static_cast<std::size_t>(nav_curr_track) < tracks.size()) {
        const std::vector<midi::ChannelEvent>& ch_evs =
            tracks[nav_curr_track].get_channel_evs();

        if (!ch_evs.empty()) {
            auto curr_ch_event =
                editor::get_next_specific_ch_ev_idx(ch_evs, channel_event_type, std::nullopt);

            while (curr_ch_event.has_value()) {
                const auto next_ch_event = editor::get_next_specific_ch_ev_idx(
                    ch_evs, channel_event_type, *curr_ch_event + 1);
                if (!next_ch_event.has_value()) {
                    break;
                }

                // fixed rust bug: coloured by the leftover loop counter and greyed like an onion
                emit(ch_evs[*curr_ch_event], ch_evs[*next_ch_event], nav_curr_track, 0u);
                curr_ch_event = next_ch_event;
            }
        }
    }

    if (handle_id != 0) {
        flush(handle_id);
    }
}

}
