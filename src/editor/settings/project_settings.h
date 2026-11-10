#pragma once

#include <array>
#include <optional>
#include <string>

#include "ui/dialogs/dialog.h"
#include "editor/project/project_manager.h"
#include "core/shared.h"

namespace andromeda::editor {

class ProjectSettings final : public app::Dialog {
public:
    ProjectSettings() = default;
    explicit ProjectSettings(ProjectManager* project_manager)
        : project_manager(project_manager) {}

    [[nodiscard]] const char* get_title() const override { return "Project Information"; }
    void draw_content() override;

    [[nodiscard]] const app::DialogActions get_actions() const override {
        return app::DialogActions::Ok;
    }

    ProjectManager* project_manager;

private:
    bool buffers_loaded_ = false;
    std::array<char, 256> name_buf_{};
    std::array<char, 256> author_buf_{};
    std::array<char, 1024> description_buf_{};
};

}
