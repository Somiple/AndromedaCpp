#pragma once

#include <memory>

#include <sol/sol.hpp>

#include "editor/project/project_manager.h"
#include "core/shared.h"

namespace andromeda::app::managers {
class PlaybackManager;
}

namespace andromeda::editor {

class AndromedaObj {
public:
    AndromedaObj(ProjectManager* project_manager,
                 app::managers::PlaybackManager* playback_manager)
        : project_manager_(project_manager), playback_manager_(playback_manager) {}

    static void register_type(sol::state& lua);

    [[nodiscard]] double ticks_to_secs(double tick) const;
    [[nodiscard]] double secs_to_ticks(double secs) const;
    [[nodiscard]] double get_ppq() const;
    [[nodiscard]] double get_playhead_tick_pos() const;
    [[nodiscard]] double get_playhead_secs_pos() const;

private:
    ProjectManager* project_manager_;
    app::managers::PlaybackManager* playback_manager_;
};

}
