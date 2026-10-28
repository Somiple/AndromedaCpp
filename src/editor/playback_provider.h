#pragma once

namespace andromeda::editor {

/// Value snapshot of the transport, sampled on demand.
/// Deliberately a plain aggregate: no pointers into playback state, so the editor layer
/// never has to reason about the lifetime or threading of the audio side.
struct PlaybackSnapshot {
    double playhead_tick = 0.0; ///< Current playhead position, in timeline ticks.
    bool playing = false;       ///< True while the transport is rolling.
};

/// Narrow, editor-owned view of the transport.
///
/// REFACTOR: the editor layer used to reach `managers::PlaybackManager` directly, which is
/// a ui/-layer type (ui/app/managers.h) and therefore an upward dependency. Components now
/// depend on this interface only; the ui layer supplies an adapter at the single
/// construction site. The surface is exactly the subset the editor actually uses — nothing
/// speculative is declared here.
class IPlaybackProvider {
public:
    virtual ~IPlaybackProvider() = default;

    /// @return A value snapshot of the current transport state.
    [[nodiscard]] virtual PlaybackSnapshot snapshot() const = 0;

    /// Moves the playhead to @p tick (the implementation clamps to the valid range).
    virtual void seek(double tick) = 0;

    /// @return Tick at which the current playback run started (the seek point).
    [[nodiscard]] virtual double playback_start_tick() const = 0;

    /// Starts (@c true) or stops (@c false) transport playback.
    virtual void set_playing(bool playing) = 0;
};

} // namespace andromeda::editor
