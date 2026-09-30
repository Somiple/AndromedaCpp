#pragma once

#include <memory>

#include <sol/sol.hpp>

#include "editor/playhead.h"
#include "editor/project/project_manager.h"
#include "util/shared.h"

namespace andromeda::editor {

class AndromedaObj {
public:
    AndromedaObj(util::SharedPtr<ProjectManager> project_manager,
                 std::shared_ptr<Playhead> playhead)
        : project_manager_(std::move(project_manager)), playhead_(std::move(playhead)) {}

    static void register_type(sol::state& lua);

    [[nodiscard]] double ticks_to_secs(double tick) const;
    [[nodiscard]] double secs_to_ticks(double secs) const;
    [[nodiscard]] double get_ppq() const;
    [[nodiscard]] double get_playhead_tick_pos() const;
    [[nodiscard]] double get_playhead_secs_pos() const;

private:
    util::SharedPtr<ProjectManager> project_manager_;
    std::shared_ptr<Playhead> playhead_;
};

}
