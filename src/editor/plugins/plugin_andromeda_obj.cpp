#include "editor/plugins/plugin_andromeda_obj.h"

#include <shared_mutex>

#include "editor/timeline/timeline.h"
#include "app/managers.h"

namespace andromeda::editor {

namespace {
using andromeda::app::managers::PlaybackManager;
}

void AndromedaObj::register_type(sol::state& lua) {
    lua.new_usertype<AndromedaObj>("AndromedaObj", sol::no_constructor,
                                   "ticks_to_secs", &AndromedaObj::ticks_to_secs,
                                   "secs_to_ticks", &AndromedaObj::secs_to_ticks,
                                   "get_ppq", &AndromedaObj::get_ppq,
                                   "get_playhead_tick_pos",
                                   &AndromedaObj::get_playhead_tick_pos,
                                   "get_playhead_secs_pos",
                                   &AndromedaObj::get_playhead_secs_pos);
}

double AndromedaObj::ticks_to_secs(double tick) const {
    Timeline* timeline = project_manager_->get_timeline();
    return timeline->tick_to_secs(static_cast<MIDITick>(tick));
}

double AndromedaObj::secs_to_ticks(double secs) const {
    Timeline* timeline = project_manager_->get_timeline();
    return timeline->secs_to_tick(secs);
}

double AndromedaObj::get_ppq() const {
    return project_manager_->get_ppq();
}

double AndromedaObj::get_playhead_tick_pos() const {
    const double secs = playback_manager_->get_playback_start();
    Timeline* timeline = project_manager_->get_timeline();
    return static_cast<double>(timeline->secs_to_tick(secs));
}

double AndromedaObj::get_playhead_secs_pos() const {
    return playback_manager_ ? playback_manager_->get_playback_start() : 0.0;
}

}
