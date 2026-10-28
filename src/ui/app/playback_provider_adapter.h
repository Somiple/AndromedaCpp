#pragma once

#include "editor/playback_provider.h"

namespace andromeda::app {

namespace managers {
class PlaybackManager;
}

/// Adapts `managers::PlaybackManager` to the editor layer's `editor::IPlaybackProvider`.
///
/// REFACTOR: this is the only type that knows both sides of the dependency. The editor
/// layer sees just the narrow interface, while the transport implementation stays in the
/// ui layer. Owned by EditorApp and outlives every EditorContext that points at it.
class PlaybackProviderAdapter final : public editor::IPlaybackProvider {
public:
    /// @param playback Backing transport manager; must outlive this adapter. Not null.
    explicit PlaybackProviderAdapter(managers::PlaybackManager& playback) noexcept;

    /// @return Playhead tick and rolling state of the transport.
    [[nodiscard]] editor::PlaybackSnapshot snapshot() const override;

    /// Moves the playhead. @param tick Target position in timeline ticks.
    void seek(double tick) override;

    /// @return Tick at which the current playback run started.
    [[nodiscard]] double playback_start_tick() const override;

    /// Starts or stops transport playback. @param playing Desired rolling state.
    void set_playing(bool playing) override;

private:
    managers::PlaybackManager& _playback; ///< Non-owning reference to the transport manager.
};

} // namespace andromeda::app
