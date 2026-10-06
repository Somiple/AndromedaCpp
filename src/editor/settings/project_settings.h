#pragma once

#include <array>
#include <optional>
#include <string>

#include "app/ui/dialog.h"
#include "editor/project/project_manager.h"
#include "util/shared.h"

namespace andromeda::editor {

class ProjectSettings final : public app::Dialog {
public:
    ProjectSettings() = default;
    explicit ProjectSettings(ProjectManager* project_manager)
        : project_manager(project_manager) {}

    app::MaybeDlgAction draw(const app::ImageResources& images) override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return app::dialog_names::DIALOG_NAME_PROJECT_SETTINGS;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return "Project Information"; }
    [[nodiscard]] std::uint16_t get_flags() const override {
        return app::dialog_flags::DIALOG_NO_COLLAPSABLE | app::dialog_flags::DIALOG_NO_RESIZABLE;
    }
    [[nodiscard]] std::optional<app::DialogActionButtons> get_action_buttons() const override {
        return app::DlgOk{app::dialog_default_close_action()};
    }

    ProjectManager* project_manager;

private:
    bool buffers_loaded_ = false;
    std::array<char, 256> name_buf_{};
    std::array<char, 256> author_buf_{};
    std::array<char, 1024> description_buf_{};
};

}
