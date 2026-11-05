#include "ui/rendering/data_view.h"
#include "ui/app/main_window.h"
#include <algorithm>
#include <bit>
#include <mutex>
#include <shared_mutex>
#include <format>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>
#include "audio/audio_timer.h"

#include "core/debugger.h"
#include "ui/rendering/note_cull_helper.h"
#include "ui/rendering/piano_roll.h"
#include "editor/util.h"
#include "ui/app/editor_app.h"
#include "core/colors.h"

namespace andromeda::app::rendering {

using midi::Note;
using util::Debugger;

namespace {

constexpr int RESIDENT_POINTS_UNIT = 6;
constexpr int RESIDENT_PALETTE_UNIT = 7;
constexpr std::size_t RESIDENT_MAX_PALETTE = 1u << 20;
constexpr std::size_t RESIDENT_VERTICES_PER_POINT = 12;         // stem quad + cap quad
constexpr std::size_t RESIDENT_MAX_POINTS_PER_DRAW = 16u * 1024u * 1024u;
constexpr std::size_t RESIDENT_UPLOAD_NOTES_PER_FRAME = 1u << 20;  // 16 MB of notes per frame
constexpr std::size_t CPU_FALLBACK_HANDLES_PER_PIXEL = 4;
constexpr std::size_t NOTE_TEXEL_BYTES = 4 * sizeof(std::uint32_t);

bool resident_disabled() {
    static const bool disabled = std::getenv("ANDROMEDA_DV_NO_RESIDENT") != nullptr;
    return disabled;
}

// the shader works in signed 32-bit tick space
std::uint32_t clamp_tick(editor::MIDITick t) {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(t), 0x7FFFFFFFull));
}

void release_resident(GLuint& buf, GLuint& tex) {
    if (tex != 0) { glDeleteTextures(1, &tex); tex = 0; }
    if (buf != 0) { glDeleteBuffers(1, &buf); buf = 0; }
}

#pragma region shaders
constexpr const char* data_view_resident_vert = R"(#version 330 core

// Vertex-pulled velocity notes. No vertex attributes: draw with glDrawArrays and 12 vertices per
// note (6 for the stem, 6 for the cap). Output is in the panel's 0..1 space.

uniform usamplerBuffer noteData;  // RGBA32UI: start tick, end tick, velocity, channel
uniform usamplerBuffer palette;   // R32UI: packed 0xRRGGBB per (track << 4) | channel

uniform float width;
uniform float height;
uniform float zoomTicks;
uniform int   tickBase;           // floor(view start tick)
uniform float tickFrac;           // view start tick - tickBase
uniform int   firstNote;
uniform int   noteStride;         // density cap: draw every Nth note
uniform int   trackKey;           // track << 4
uniform int   ghostMode;          // 0 none, 1 partial colour, 2 grey

flat out vec3 vColor;

vec3 unpack_rgb(uint c) {
    return vec3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0;
}

float snap(float v, float size) { return floor(v * size + 0.5) / size; }

const vec2 QUAD[6] = vec2[6](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
    int note = firstNote + (gl_VertexID / 12) * noteStride;
    int corner = gl_VertexID % 12;

    uvec4 n = texelFetch(noteData, note);

    // ticks relative to the view start
    float startT = float(int(n.x) - tickBase) - tickFrac;
    float endT = max(float(int(n.y) - tickBase) - tickFrac, startT);

    float x0 = snap(startT / zoomTicks, width);
    float x1 = snap(endT / zoomTicks, width);
    float y  = snap(clamp((float(n.z) - 1.0) / 126.0, 0.0, 1.0), height);

    float halfW = 1.0 / width;    // 2 px stem
    float halfH = 1.0 / height;   // 2 px cap

    vec4 rect; // min.xy, max.xy
    if (corner < 6) {
        rect = vec4(x0 - halfW, 0.0, x0 + halfW, y);        // stem: baseline up to the velocity
    } else {
        rect = vec4(x0, y - halfH, x1, y + halfH);          // cap: the note's duration
    }

    vec2 pos = mix(rect.xy, rect.zw, QUAD[corner % 6]);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);

    vec3 color = unpack_rgb(texelFetch(palette, trackKey + int(n.w & 15u)).x);
    if (ghostMode == 1)      color = mix(color, vec3(127.0 / 255.0), 0.5);
    else if (ghostMode == 2) color = vec3(127.0 / 255.0);
    vColor = color;
})";

constexpr const char* data_view_resident_frag = R"(#version 330 core

flat in vec3 vColor;
out vec4 outColor;

void main() {
    outColor = vec4(vColor, 1.0);
})";

#pragma endregion

} // namespace

DataViewRenderer::DataViewRenderer(app::MainWindow* app) : Renderer(app) {
    dv_resident_program_ = ShaderProgram::create("Data View Resident",
        data_view_resident_vert, data_view_resident_frag);
    dv_resident_vao_ = VertexArray::create();
}

DataViewRenderer::~DataViewRenderer() {
    for (auto& [track, res] : note_res_) {
        release_resident(res.buf, res.tex);
    }
    release_resident(palette_.buf, palette_.tex);
}

std::uint64_t DataViewRenderer::handles_key(std::vector<midi::MIDITrack>& tracks,
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
    mix(static_cast<std::uint64_t>(_app->colors().get_index_type()));
    mix(_app->colors().version());
    editor::SharedSelectedNotes* selection = _app->controller().get_selection();
    mix(selection ? selection->version() : 0);

    mix(tracks.size());
    for (midi::MIDITrack& track : tracks) {
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
    // note: somi will have to redo the handle cache... we now use a rect batch framework to draw stuff.

    /*glUseProgram(dv_cache_program_.id());
    dv_cache_program_.set_int("handles", 0);
    dv_direct_vao_.bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, handle_cache_.tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    _app->colors().get_texture().bind();*/
}

// TODO: have this get the same time as the piano roll time.
// once we implement the individual components (piano roll class, track view class, dava view class, etc.)
float DataViewRenderer::get_time() const {
    ViewSettings* view_settings = &_app->settings();
    if (view_settings->pr_autoscroll) {
        auto* timer = _app->get_editor_app()->get_timer();
        if (timer->is_running()) {
            const auto* timeline = _app->controller().get_project_manager()->get_timeline();
            return static_cast<float>(timeline->secs_to_tick(timer->elapsed()));
        }
        return _app->navigation()->value.tick_pos_smoothed;
    }

    return _app->navigation()->value.tick_pos_smoothed;
}

DataViewRenderer::OnionRange DataViewRenderer::onion_range(std::size_t track_count,
                                                           std::uint16_t nav_curr_track) const {
    ViewSettings* view_settings = &_app->settings();
    VS_PianoRoll_OnionState onion_state = view_settings->pr_onion_state;
    VS_PianoRoll_OnionColoring onion_coloring = view_settings->pr_onion_coloring;

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
    if (window_size_.x <= 0.0f || window_size_.y <= 0.0f) return;

    {
        ViewSettings* view_settings = &_app->settings();
        if (view_settings->pr_dataview_state == VS_PianoRoll_DataViewState::Hidden) {
            return;
        }
    }

    const float tick_pos = get_time();

    float zoom_ticks = 0.0f;
    {
        editor::PianoRollNavigation& nav = _app->navigation()->value;
        zoom_ticks = nav.zoom_ticks_smoothed;
    }

    {
        const auto* timer = _app->get_editor_app()->get_timer();
        bool autoscroll = _app->settings().pr_autoscroll;

        // fixed rust bug: taken once at play start and scaled on zoom, so it drifted from the
        // playhead line; it is now the same live formula as the piano roll
        view_offset_ = 0.0f;
        if (timer->is_running() && autoscroll) {
            const auto* timeline = _app->controller().get_project_manager()->get_timeline();
            editor::PianoRollNavigation& nav = _app->navigation()->value;
            view_offset_ = nav.tick_pos_smoothed -
                           static_cast<float>(timeline->secs_to_tick(timer->get_start_time()));
        }
        // the view on screen stops at the song start, as the playhead line's does
        view_offset_ = std::max(view_offset_, -tick_pos);
    }

    const float tick_pos_offs = tick_pos + view_offset_;
    const double view_start = tick_pos_offs;
    const double view_end = tick_pos_offs + zoom_ticks;

    {
        PrimitiveScope scope(&data_view_render_batch_);

        auto& controller = _app->controller();
        auto& manager = *controller.get_project_manager();
        const auto& timeline = *manager.get_timeline();

        // TODO: move this to helper function in class
        const auto& snap_to_x = [&](float val) {
            return std::roundf(val * window_size_.x) / window_size_.x;
        };

        const auto& snap_to_y = [&](float val) {
            return std::roundf(val * window_size_.y) / window_size_.y;
            };

        std::uint32_t bar_color_base = util::BAR_BASE;
        {
            const auto& tracks = *manager.get_tracks();
            if (controller.get_active_track() < tracks.size()) {
                const auto& active_track = tracks[controller.get_active_track()];
                if (active_track.project_meta.color_coded) {
                    const auto& color_code = *active_track.get_color_code();
                    bar_color_base = util::mix_color(bar_color_base, util::pack_color(color_code), 0.5f);
                }
            }
        }

        // draw bars
        timeline.for_each_time_signature(static_cast<midi::MIDITick>(std::max(view_start, 0.0)), static_cast<midi::MIDITick>(view_end),
            [&](const editor::timeline::TimeSigPoint& s, std::optional<midi::MIDITick> next) {
                const double seg_end = next ? double(*next) : view_end + 1.0;

                const double lo = std::max(view_start, double(s.tick));
                midi::MIDITick k = midi::MIDITick((lo - s.tick) / s.bar_len);

                for (;; ++k) {
                    const double bar_t = double(s.tick) + double(k) * s.bar_len;
                    if (bar_t >= seg_end || bar_t > view_end) break;

                    const float bar_start =
                        snap_to_x(float((bar_t - view_start) / zoom_ticks));

                    const float bar_end =
                        snap_to_x(float((bar_t + double(s.bar_len) - view_start) / zoom_ticks));

                    const float bar_width = bar_end - bar_start;

                    std::uint32_t bar_color = bar_color_base;
                    if (((s.first_bar + k) & 0b1) != 0)
                        bar_color = util::mix_color(bar_color, util::BLACK, 0.2f);

                    data_view_render_batch_.push_rect(
                        { bar_start, 0.0f },
                        { bar_width, 1.0f }, bar_color);

                    for (std::size_t beat = 0; beat < s.numerator; beat++) {
                        const double beat_t = bar_t + double(beat) * s.beat_len;
                        if (beat_t >= seg_end || beat_t > view_end) break;

                        const float beat_pos =
                            snap_to_x(float((beat_t - view_start) / zoom_ticks));
                        
                        data_view_render_batch_.push_v_line(
                            beat_pos, { 0.0f, 1.0f },
                            util::mix_color(bar_color, util::BLACK, 0.5f),
                            1.5f / window_size_.x
                        );

                        const float step_length_ticks = static_cast<float>(timeline.step_length_at(beat_t));
                        if (step_length_ticks <= 1.0f) continue;

                        for (double sub_t = beat_t + step_length_ticks;
                            sub_t < beat_t + s.beat_len && sub_t < seg_end && sub_t <= view_end;
                            sub_t += step_length_ticks) {

                            if (sub_t < view_start)
                                continue;

                            const float sub_pos = snap_to_x(float((sub_t - view_start) / zoom_ticks));

                            data_view_render_batch_.push_v_line(
                                sub_pos, { 0.0f, 1.0f },
                                util::mix_color(bar_color, util::BLACK, 0.25f),
                                1.5f / window_size_.x
                            );
                        }
                    }

                    if ((bar_t - view_start) >= 0.0f) {
                        data_view_render_batch_.push_v_line(
                            bar_start, { 0.0f, 1.0f },
                            util::mix_color(bar_color, util::BLACK, 0.5),
                            2.0f / window_size_.x
                        );
                    }
                }
            });

        draw_lane_handles(tick_pos, zoom_ticks);
    }

    // the batch has flushed by now, so the resident handles land on top of the bars
    draw_velocity_resident();

    /* {
        glUseProgram(dv_program_.id());

        dv_vertex_array_.bind();
        dv_instance_buffer_.bind();
        dv_vertex_buffer_.bind();
        dv_index_buffer_.bind();

        dv_program_.set_float("width", window_size_.x);
        dv_program_.set_float("height", window_size_.y);
        {
            dv_program_.set_float("ppqNorm", static_cast<float>(_app->controller().get_project_manager()->get_ppq()) / zoom_ticks);
        }

        float curr_bar_tick = 0.0f;
        std::size_t bar_num = 0;
        std::size_t bar_id = 0;

        auto* project_manager = _app->controller().get_project_manager();
        const auto& timeline = *project_manager->get_timeline();

        const double view_start = tick_pos_offs;
        const double view_end = tick_pos_offs + zoom_ticks;

        auto flush = [&] {
            if (bar_id == 0) return;
            dv_instance_buffer_.set_data(
                std::span<const RenderDataViewBar>(bars_render_.data(), bar_id), GL_DYNAMIC_DRAW);
            glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, (GLsizei)bar_id);
            bar_id = 0;
            };

        timeline.for_each_time_signature(
            static_cast<midi::MIDITick>(std::max(view_start, 0.0)), static_cast<midi::MIDITick>(view_end),
            [&](const editor::timeline::TimeSigPoint& s, std::optional<midi::MIDITick> next) {
                const double seg_end = next ? double(*next) : view_end + 1.0;
                const float  width = float(s.beat_len * s.numerator) / zoom_ticks; 

                // first bar that can be visible: the one containing view_start (or the segment's first)
                const double lo = std::max(view_start, double(s.tick));
                midi::MIDITick k = midi::MIDITick((lo - s.tick) / s.bar_len);

                for (;; ++k) {
                    const double bar_t = double(s.tick) + double(k) * s.bar_len;
                    if (bar_t >= seg_end || bar_t > view_end) break;

                    bars_render_[bar_id++] = RenderDataViewBar{
                        float((bar_t - view_start) / zoom_ticks),
                        width,
                        uint32_t(s.first_bar + k) };          // real bar index

                    if (bar_id == bars_render_.size()) flush();
                }
            });
        flush();

        glUseProgram(0);
    }

    {
        glUseProgram(dv_handles_program_.id());

        glActiveTexture(GL_TEXTURE0);
        _app->colors().get_texture().bind();

        dv_handles_program_.set_int("noteColorTexture", 0);
        dv_handles_program_.set_float("width", window_size_.x);
        dv_handles_program_.set_float("height", window_size_.y);

        const auto* source = _app->controller().get_data_editing()->get_edit_source();
        switch (source->event_source) {
        case editor::EventSourceKind::Velocity:
            draw_note_velocities(tick_pos, zoom_ticks);
            break;
        default:
            draw_lane_handles(tick_pos, zoom_ticks);
            break;
        }

        glUseProgram(0);
    }*/
}

void DataViewRenderer::set_direct_frame_uniforms(float tick_pos, float zoom_ticks) {
    /*int color_mode = 0;
    switch (_app->colors().get_index_type()) {
    case NoteColorIndexing::Channel:      color_mode = 0; break;
    case NoteColorIndexing::Track:        color_mode = 1; break;
    case NoteColorIndexing::ChannelTrack: color_mode = 2; break;
    }

    const auto* timer = _app->get_editor_app()->get_timer();
    const bool is_playing = timer->is_running();

    const auto* timeline = _app->controller().get_project_manager()->get_timeline();
    const float playback_pos = static_cast<float>(timeline->secs_to_tick(timer->elapsed()));

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
    direct_state_.color_mode = color_mode;*/
}

bool DataViewRenderer::draw_track_direct(const VisibleTrack& visible,
                                         const std::vector<Note>& notes, std::uint64_t revision,
                                         std::uint32_t onion_meta, bool is_current_track) {
    /*const std::size_t track = visible.track;
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

    auto* selection = _app->controller().get_selection();
    const std::vector<std::size_t>* sel_ids =
        (is_current_track && selection) ? selection->get_selected_ids_in_track(
                                              static_cast<std::uint16_t>(track))
                                        : nullptr;
    const std::uint64_t selection_version = selection ? selection->version() : 0;
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
    }*/

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

editor::MIDITick DataViewRenderer::get_lane_point_length(
    const editor::EventLanePoint& point, const LaneRenderContext& ctx) const {
    if (point.carries_length()) { return point.length(); }
    if (point.has_next) { return point.next_tick - point.tick; }
    return std::max(static_cast<editor::MIDITick>(ctx.view_end), point.tick) - point.tick;
}

bool DataViewRenderer::draw_lane_handles(float tick_pos, float zoom_ticks) {
    if (!lane_ || zoom_ticks <= 0.0f ||
        window_size_.x <= 0.0f || window_size_.y <= 0.0f) {
        return false;
    }

    const auto* source =
        _app->controller().get_data_editing()->get_edit_source();

    if (!source) return false;

    const auto* source_info =
        editor::EventSources::get_source(source->event_source);

    if (!source_info) return false;

    LaneRenderContext ctx;
    ctx.tick_pos_offs = tick_pos + view_offset_;
    ctx.view_end = ctx.tick_pos_offs + zoom_ticks;
    ctx.zoom_ticks = zoom_ticks;
    // TODO: have source report the baseline instead
    ctx.baseline = source->event_source == editor::EventSourceKind::PitchBend
        ? 0.5f : 0.0f;
    ctx.is_global_lane = source_info->info.source_origin == editor::EventSourceOrigin::Global;

    if (ctx.baseline > 0.0f && ctx.baseline < 1.0f) {
        data_view_render_batch_.push_h_line(
            { 0.0f, 1.0f },
            std::roundf(ctx.baseline * window_size_.y) / window_size_.y,
            util::WHITE,
            1.0f / window_size_.y
        );
    }

    if (source->event_source == editor::EventSourceKind::Velocity) {
        // the notes are the lane: no projection, no copy
        draw_velocity_lane(ctx);
        return true;
    }

    const auto& points = lane_->points();
    const auto& ghosts = lane_->ghosts();

    if (points.empty() && ghosts.empty()) return true;

    draw_continuous_lane(ghosts, ctx, true);
    draw_continuous_lane(points, ctx, false);

    return true;
}

void DataViewRenderer::draw_lane_point(const editor::EventLanePoint& point,
    editor::MIDITick length, float start_value, const LaneRenderContext& ctx,
    bool is_ghost) {

    const auto& snap_to_x = [&](float val) {
        return std::roundf(val * window_size_.x) / window_size_.x;
        };

    const auto& snap_to_y = [&](float val) {
        return std::roundf(val * window_size_.y) / window_size_.y;
        };

    const std::size_t trk_chan = (static_cast<std::size_t>(point.track) << 4) | point.channel;
    const auto& colors = _app->colors();
    std::uint32_t lane_color = ctx.is_global_lane ?
        0xA07FFF :
        util::pack_color(colors.get_colors()[colors.get_index(trk_chan)]);

    if (is_ghost) {
        switch (_app->view_settings_handle()->value.pr_onion_coloring) {
        case VS_PianoRoll_OnionColoring::PartialColor: {
            lane_color = util::mix_color(lane_color, 0x7F7F7F, 0.5f);
            break;
        }
        case VS_PianoRoll_OnionColoring::GrayedOut: {
            lane_color = 0x7F7F7F;
            break;
        }
        default: break;
        }
    }

    float start_x = snap_to_x((point.tick - ctx.tick_pos_offs) / ctx.zoom_ticks);
    float end_x = snap_to_x((point.tick + length - ctx.tick_pos_offs) / ctx.zoom_ticks);

    float start_y = 0.0f, end_y = 0.0f;
    start_y = snap_to_y(start_value);
    end_y = snap_to_y(point.value);

    if (end_y < start_y) std::swap(end_y, start_y);

    if (start_x > end_x) return;

    if (start_y != end_y) {
        data_view_render_batch_.push_v_line(
            start_x, { start_y, end_y },
            lane_color, 2.0f / window_size_.x);
    }

    data_view_render_batch_.push_h_line(
        { start_x, end_x }, snap_to_y(point.value),
        lane_color, 2.0f / window_size_.y);
}

void DataViewRenderer::sync_palette(std::uint32_t max_key) {
    const auto& colors = _app->colors();
    const std::size_t entries =
        std::min<std::size_t>(static_cast<std::size_t>(max_key) + 1, RESIDENT_MAX_PALETTE);
    const auto index_type = static_cast<std::uint64_t>(colors.get_index_type());
    const auto version = static_cast<std::uint64_t>(colors.version());

    if (palette_.tex != 0 && palette_.entries >= entries &&
        palette_.version == version && palette_.index_type == index_type) {
        return;
    }

    resident_staging_.resize(entries);
    for (std::size_t k = 0; k < entries; ++k) {
        resident_staging_[k] =
            static_cast<std::uint32_t>(util::pack_color(colors.get_colors()[colors.get_index(k)]));
    }

    if (palette_.buf == 0) glGenBuffers(1, &palette_.buf);
    if (palette_.tex == 0) glGenTextures(1, &palette_.tex);

    glBindBuffer(GL_TEXTURE_BUFFER, palette_.buf);
    glBufferData(GL_TEXTURE_BUFFER,
        static_cast<GLsizeiptr>(entries * sizeof(std::uint32_t)),
        resident_staging_.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);

    GLint prev_active = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0 + RESIDENT_PALETTE_UNIT);
    glBindTexture(GL_TEXTURE_BUFFER, palette_.tex);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, palette_.buf);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glActiveTexture(static_cast<GLenum>(prev_active));

    palette_.entries = entries;
    palette_.version = version;
    palette_.index_type = index_type;
}

bool DataViewRenderer::sync_note_resident(NoteResident& r, const std::vector<Note>& notes,
    std::uint64_t revision, std::size_t& budget) {
    static const GLint max_texels = [] {
        GLint v = 0;
        glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &v);
        return v;
    }();

    const std::size_t total = notes.size();

    if (r.data != notes.data() || r.total != total || r.revision != revision) {
        // same note count: overwrite in place and keep drawing the old contents meanwhile, so an
        // edit on a huge track doesn't blank the strip while it streams back in
        const bool overwrite = r.buf != 0 && r.total == total && !r.too_large && total != 0;
        r.data = notes.data();
        r.revision = revision;
        r.uploaded = 0;
        r.overwrite = overwrite;

        if (!overwrite) {
            r.total = total;
            r.drawable = 0;
            r.too_large = total > static_cast<std::size_t>(max_texels);
            if (r.too_large) {
                Debugger::log_warning("Track too large for a resident velocity buffer; using the CPU path.");
            } else if (total != 0) {
                if (r.buf == 0) glGenBuffers(1, &r.buf);
                if (r.tex == 0) glGenTextures(1, &r.tex);

                glBindBuffer(GL_TEXTURE_BUFFER, r.buf);
                glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(total * NOTE_TEXEL_BYTES),
                    nullptr, GL_DYNAMIC_DRAW);
                glBindBuffer(GL_TEXTURE_BUFFER, 0);

                GLint prev_active = 0;
                glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
                glActiveTexture(GL_TEXTURE0 + RESIDENT_POINTS_UNIT);
                glBindTexture(GL_TEXTURE_BUFFER, r.tex);
                glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, r.buf);
                glBindTexture(GL_TEXTURE_BUFFER, 0);
                glActiveTexture(static_cast<GLenum>(prev_active));
            }
        }
    }

    if (r.too_large) return false;
    if (r.total == 0 || r.uploaded >= r.total) return true;
    if (budget == 0) {
        uploading_ = true;
        return true;
    }

    const std::size_t n = std::min(budget, r.total - r.uploaded);
    resident_staging_.resize(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        const Note& note = notes[r.uploaded + i];
        std::uint32_t* out = &resident_staging_[i * 4];
        out[0] = clamp_tick(note.start);
        out[1] = clamp_tick(note.start + note.length);
        out[2] = static_cast<std::uint32_t>(note.get_velocity());
        out[3] = static_cast<std::uint32_t>(note.get_channel());
    }

    glBindBuffer(GL_TEXTURE_BUFFER, r.buf);
    glBufferSubData(GL_TEXTURE_BUFFER, static_cast<GLintptr>(r.uploaded * NOTE_TEXEL_BYTES),
        static_cast<GLsizeiptr>(n * NOTE_TEXEL_BYTES), resident_staging_.data());
    glBindBuffer(GL_TEXTURE_BUFFER, 0);

    r.uploaded += n;
    budget -= n;

    if (r.uploaded >= r.total) {
        r.drawable = r.total;
        r.overwrite = false;
    } else {
        // notes are start-ordered, so the uploaded prefix is a valid partial lane
        if (!r.overwrite) r.drawable = r.uploaded;
        uploading_ = true;
    }
    return true;
}

void DataViewRenderer::draw_velocity_cpu(std::size_t track, std::size_t first, std::size_t end,
    std::size_t stride, const std::vector<Note>& notes, int ghost_mode,
    const LaneRenderContext& ctx) {
    const auto snap_to_x = [&](float val) { return std::roundf(val * window_size_.x) / window_size_.x; };
    const auto snap_to_y = [&](float val) { return std::roundf(val * window_size_.y) / window_size_.y; };

    // the CPU path is only a safety net: keep it to a few handles per pixel column
    const std::size_t columns = std::max<std::size_t>(1, static_cast<std::size_t>(window_size_.x));
    const std::size_t budget = columns * CPU_FALLBACK_HANDLES_PER_PIXEL;
    const std::size_t n = end - first;
    stride = std::max(stride, (n + budget - 1) / budget);

    const auto& colors = _app->colors();
    for (std::size_t i = first; i < end; i += stride) {
        const Note& note = notes[i];

        const std::size_t key = (track << 4) | static_cast<std::size_t>(note.get_channel());
        std::uint32_t color = util::pack_color(colors.get_colors()[colors.get_index(key)]);
        if (ghost_mode == 1) color = util::mix_color(color, 0x7F7F7F, 0.5f);
        else if (ghost_mode == 2) color = 0x7F7F7F;

        const float x0 = snap_to_x((static_cast<float>(note.start) - ctx.tick_pos_offs) / ctx.zoom_ticks);
        const float x1 = snap_to_x((static_cast<float>(note.start + note.length) - ctx.tick_pos_offs) / ctx.zoom_ticks);
        const float value = std::clamp((static_cast<float>(note.get_velocity()) - 1.0f) / 126.0f, 0.0f, 1.0f);
        const float y = snap_to_y(value);

        if (y > 0.0f) {
            data_view_render_batch_.push_v_line(x0, { 0.0f, y }, color, 2.0f / window_size_.x);
        }
        data_view_render_batch_.push_h_line({ x0, std::max(x0, x1) }, y, color, 2.0f / window_size_.y);
    }
}

void DataViewRenderer::draw_velocity_lane(const LaneRenderContext& ctx) {
    velocity_draws_.clear();
    velocity_pending_ = false;
    uploading_ = false;

    editor::ProjectManager* manager = _app->controller().get_project_manager();
    std::vector<midi::MIDITrack>* tracks_ptr = manager ? manager->get_tracks() : nullptr;
    if (tracks_ptr == nullptr || tracks_ptr->empty()) return;
    std::vector<midi::MIDITrack>& tracks = *tracks_ptr;

    // drop residents of tracks that no longer exist
    for (auto it = note_res_.begin(); it != note_res_.end();) {
        if (it->first >= tracks.size()) {
            release_resident(it->second.buf, it->second.tex);
            it = note_res_.erase(it);
        } else {
            ++it;
        }
    }

    const std::uint16_t lane_track = _app->controller().get_data_editing()->lane_track();
    const OnionRange onion = onion_range(tracks.size(), lane_track);
    const bool gpu_ok = !resident_disabled() && dv_resident_program_.id() != 0;

    // draw order: onion ghosts first, the editable track on top
    std::vector<std::uint16_t> order;
    for (std::size_t t = onion.begin; t < onion.end; ++t) {
        if (t != lane_track) order.push_back(static_cast<std::uint16_t>(t));
    }
    if (lane_track < tracks.size()) order.push_back(lane_track);

    // upload the editable track first so it is the one that is complete soonest
    if (gpu_ok) {
        std::size_t budget = RESIDENT_UPLOAD_NOTES_PER_FRAME;
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const std::vector<Note>& notes = tracks[*it].get_notes();
            if (notes.empty()) continue;
            sync_note_resident(note_res_[*it], notes, tracks[*it].revision, budget);
        }
    }

    auto& culler = _app->note_culler();
    culler->sync_cull_array_lengths(tracks);

    struct Meta { int ghost_mode; bool gpu; };
    std::vector<VisibleTrack> visible;
    std::vector<Meta> meta;

    for (const std::uint16_t t : order) {
        const std::vector<Note>& notes = tracks[t].get_notes();
        if (notes.empty()) continue;

        culler->update_cull_for_track(tracks, t, ctx.tick_pos_offs, ctx.zoom_ticks, false);
        const auto [cull_a, cull_b] = culler->get_track_cull_range(t);
        std::size_t first = cull_a;
        std::size_t last = cull_b;
        if (last > notes.size()) {
            culler->update_cull_for_track(tracks, t, ctx.tick_pos_offs, ctx.zoom_ticks, true);
            const auto [retry_a, retry_b] = culler->get_track_cull_range(t);
            first = retry_a;
            last = retry_b;
        }
        last = std::min(last, notes.size());

        bool gpu = false;
        if (gpu_ok) {
            const auto res = note_res_.find(t);
            if (res != note_res_.end() && !res->second.too_large) {
                gpu = true;
                last = std::min(last, res->second.drawable); // only what has reached the GPU
            }
        }
        if (last <= first) continue;

        visible.push_back(VisibleTrack{ t, first, last, 1 });
        meta.push_back({ t == lane_track ? 0 : static_cast<int>(onion.color_meta), gpu });
    }

    apply_density_cap(visible);

    for (std::size_t i = 0; i < visible.size(); ++i) {
        const VisibleTrack& v = visible[i];
        if (meta[i].gpu) {
            velocity_draws_.push_back(VelocityDraw{ v.track, v.first, v.end, v.stride, meta[i].ghost_mode });
        } else {
            draw_velocity_cpu(v.track, v.first, v.end, v.stride, tracks[v.track].get_notes(),
                meta[i].ghost_mode, ctx);
        }
    }

    if (!velocity_draws_.empty()) {
        sync_palette((static_cast<std::uint32_t>(tracks.size()) << 4) | 0xFu);
        velocity_ctx_ = ctx;
        velocity_pending_ = true;
    }
}

void DataViewRenderer::draw_velocity_resident() {
    if (!velocity_pending_) return;
    velocity_pending_ = false;

    const LaneRenderContext& ctx = velocity_ctx_;

    // relative to the view start, in whole ticks + a fraction, so large absolute ticks keep
    // full float precision on the GPU
    const float tick_base = std::floor(ctx.tick_pos_offs);
    const float tick_frac = ctx.tick_pos_offs - tick_base;

    GLint prev_active = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);

    glUseProgram(dv_resident_program_.id());
    dv_resident_vao_.bind();

    dv_resident_program_.set_int("noteData", RESIDENT_POINTS_UNIT);
    dv_resident_program_.set_int("palette", RESIDENT_PALETTE_UNIT);
    dv_resident_program_.set_float("width", window_size_.x);
    dv_resident_program_.set_float("height", window_size_.y);
    dv_resident_program_.set_float("zoomTicks", ctx.zoom_ticks);
    dv_resident_program_.set_int("tickBase", static_cast<int>(tick_base));
    dv_resident_program_.set_float("tickFrac", tick_frac);

    if (palette_.tex != 0) {
        glActiveTexture(GL_TEXTURE0 + RESIDENT_PALETTE_UNIT);
        glBindTexture(GL_TEXTURE_BUFFER, palette_.tex);
    }

    for (const VelocityDraw& d : velocity_draws_) {
        const auto res = note_res_.find(static_cast<std::uint16_t>(d.track));
        if (res == note_res_.end() || res->second.tex == 0) continue;

        glActiveTexture(GL_TEXTURE0 + RESIDENT_POINTS_UNIT);
        glBindTexture(GL_TEXTURE_BUFFER, res->second.tex);

        dv_resident_program_.set_int("ghostMode", d.ghost_mode);
        dv_resident_program_.set_int("trackKey", static_cast<int>(d.track << 4));
        dv_resident_program_.set_int("noteStride", static_cast<int>(d.stride));

        const std::size_t count = (d.end - d.first + d.stride - 1) / d.stride;
        for (std::size_t done = 0; done < count; done += RESIDENT_MAX_POINTS_PER_DRAW) {
            const std::size_t run = std::min(RESIDENT_MAX_POINTS_PER_DRAW, count - done);
            dv_resident_program_.set_int("firstNote", static_cast<int>(d.first + done * d.stride));
            glDrawArrays(GL_TRIANGLES, 0,
                static_cast<GLsizei>(run * RESIDENT_VERTICES_PER_POINT));
        }
    }

    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0 + RESIDENT_PALETTE_UNIT);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glActiveTexture(static_cast<GLenum>(prev_active));
    glBindVertexArray(0);
    glUseProgram(0);
}

void DataViewRenderer::draw_continuous_lane(const std::vector<editor::EventLanePoint>& points,
    const LaneRenderContext& ctx,
    bool is_ghost) {

    struct PendingSegment {
        const editor::EventLanePoint* point = nullptr;

        float draw_value = 0.0f;
        float latest_value = 0.0f;
        float start_value = 0.0f;

        editor::MIDITick end_tick = 0;
        bool has_successor = false;
    };

    float last_value = ctx.baseline;
    const float pixel_threshold = 1.0f / window_size_.y;

    PendingSegment pending;

    const auto flush = [&]() {
        if (!pending.point) return;

        draw_lane_point(
            *pending.point, pending.end_tick - pending.point->tick,
            pending.start_value, ctx, is_ghost);

        last_value = pending.has_successor ? pending.latest_value : ctx.baseline;

        pending.point = nullptr;
    };

    for (const auto& p : points) {
        if (static_cast<double>(p.tick) > ctx.view_end) break;

        if (p.has_next && static_cast<double>(p.next_tick) < ctx.tick_pos_offs) {
            flush();
            last_value = p.value;
            continue;
        }

        const editor::MIDITick end_tick = get_lane_point_length(p, ctx) + p.tick;

        if (pending.point) {
            const long draw_y = std::lround(pending.draw_value * window_size_.y);
            const long point_y = std::lround(p.value * window_size_.y);

            if (draw_y == point_y) {
                pending.end_tick = end_tick;
                pending.latest_value = p.value;
                pending.has_successor = p.has_next;
                continue;
            }

            flush();
        }
        
        pending.point = &p;
        pending.draw_value = p.value;
        pending.latest_value = p.value;
        pending.start_value = last_value;
        pending.end_tick = end_tick;
        pending.has_successor = p.has_next;
    }

    flush();
}

}