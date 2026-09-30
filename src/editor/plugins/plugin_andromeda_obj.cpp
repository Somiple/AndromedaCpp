#include "editor/plugins/plugin_andromeda_obj.h"

#include <shared_mutex>

#include "editor/tempo_map.h"

namespace andromeda::editor {

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
    std::shared_lock pm_lock(project_manager_->mutex);
    const ProjectManager& pm = project_manager_->value;
    const auto tempo_map = pm.get_tempo_map();
    std::shared_lock lock(tempo_map->mutex);
    return tempo_map->value.ticks_to_secs_from_map(pm.get_ppq(), static_cast<MIDITick>(tick));
}

double AndromedaObj::secs_to_ticks(double secs) const {
    std::shared_lock pm_lock(project_manager_->mutex);
    const ProjectManager& pm = project_manager_->value;
    const auto tempo_map = pm.get_tempo_map();
    std::shared_lock lock(tempo_map->mutex);
    return tempo_map->value.secs_to_ticks_from_map(pm.get_ppq(), static_cast<float>(secs));
}

double AndromedaObj::get_ppq() const {
    std::shared_lock pm_lock(project_manager_->mutex);
    return project_manager_->value.get_ppq();
}

double AndromedaObj::get_playhead_tick_pos() const {
    return playhead_ ? static_cast<double>(playhead_->start_tick) : 0.0;
}

double AndromedaObj::get_playhead_secs_pos() const {
    const MIDITick tick = playhead_ ? playhead_->start_tick : 0;

    std::shared_lock pm_lock(project_manager_->mutex);
    const ProjectManager& pm = project_manager_->value;
    const auto tempo_map = pm.get_tempo_map();
    std::shared_lock lock(tempo_map->mutex);
    return tempo_map->value.ticks_to_secs_from_map(pm.get_ppq(), tick);
}

}
