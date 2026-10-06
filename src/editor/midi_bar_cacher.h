#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "app/app_event_listener.h"
#include "editor/project/project_manager.h"
#include "midi/events/meta_event.h"
#include "util/shared.h"

namespace andromeda::editor {

class BarCacher : public app::AppEventListener {
public:
    explicit BarCacher(ProjectManager* project_manager)
        : project_manager(project_manager) {}

    void on_event(const app::AndromedaEvent& event) override {
        if (std::holds_alternative<app::PPQChanged>(event)) {
            clear_cache();
        }
    }

    void clear_cache() {
        bar_cache.clear();
        last_ts_index_ = 0;
    }

    std::pair<std::uint32_t, std::uint32_t> get_bar_interval(std::size_t bar_num);

    ProjectManager* project_manager;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> bar_cache;

private:
    void validate_bars_until(std::size_t target_bar);

    [[nodiscard]] std::pair<std::uint32_t, std::size_t> compute_bar_length_at(
        std::uint32_t start_tick, const std::vector<MetaEvent>* metas, std::size_t search_idx,
        std::uint16_t ppq) const;

    std::size_t last_ts_index_ = 0;
};

}
