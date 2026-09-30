#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "app/editor_tool.h"

namespace andromeda::app {

struct PPQChanged {
    std::uint16_t new_ppq;
};

struct EditorToolSettingsChanged {
    std::optional<EditorTool> new_tool;
    std::optional<std::pair<std::uint8_t, std::uint16_t>> new_snap_ratio;
};

using AndromedaEvent = std::variant<PPQChanged, EditorToolSettingsChanged>;

class AppEventListener {
public:
    virtual ~AppEventListener() = default;

    virtual void on_event(const AndromedaEvent& event) { (void)event; }
};

class EventListenerHandler {
public:
    void register_listener(std::shared_ptr<AppEventListener> listener) {
        event_listeners_.push_back(std::move(listener));
    }

    template <typename L>
    L* get_listener() {
        for (auto& listener : event_listeners_) {
            if (auto* typed = dynamic_cast<L*>(listener.get())) {
                return typed;
            }
        }
        return nullptr;
    }

    void send_event_to_listeners(const AndromedaEvent& event) {
        for (auto& listener : event_listeners_) {
            listener->on_event(event);
        }
    }

private:
    std::vector<std::shared_ptr<AppEventListener>> event_listeners_;
};

}
