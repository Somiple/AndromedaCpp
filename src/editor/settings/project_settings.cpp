#include "editor/settings/project_settings.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <format>
#include <mutex>

#include <imgui.h>

namespace andromeda::editor {

namespace {

void copy_into(std::span<char> dst, const std::string& src) {
    const std::size_t n = std::min(src.size(), dst.size() - 1);
    std::memcpy(dst.data(), src.data(), n);
    dst[n] = '\0';
}

constexpr std::array<std::uint16_t, 10> PPQ_VALUES = {96,  120, 192, 240,  384,
                                                      480, 768, 960, 1920, 3840};

}

app::MaybeDlgAction ProjectSettings::draw(const app::ImageResources&) {
    if (!project_manager) {
        return std::nullopt;
    }

    std::unique_lock lock(project_manager->mutex);
    ProjectManager& pm = project_manager->value;

    {
        ProjectInfo& project_info = pm.get_project_info_mut();

        if (!buffers_loaded_) {
            copy_into(name_buf_, project_info.name);
            copy_into(author_buf_, project_info.author);
            copy_into(description_buf_, project_info.description);
            buffers_loaded_ = true;
        }

        ImGui::TextUnformatted("Name");
        ImGui::SameLine();
        if (ImGui::InputText("##name", name_buf_.data(), name_buf_.size())) {
            project_info.name = name_buf_.data();
        }

        ImGui::TextUnformatted("Author");
        ImGui::SameLine();
        if (ImGui::InputText("##author", author_buf_.data(), author_buf_.size())) {
            project_info.author = author_buf_.data();
        }

        ImGui::TextUnformatted("Description");
        ImGui::SameLine();
        if (ImGui::InputTextMultiline("##description", description_buf_.data(),
                                      description_buf_.size())) {
            project_info.description = description_buf_.data();
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("PPQ");
    ImGui::SameLine();

    const std::uint16_t ppq = pm.get_ppq();
    if (ImGui::BeginCombo("##ppq", std::format("{}", ppq).c_str())) {
        for (const std::uint16_t value : PPQ_VALUES) {
            const bool selected = value == ppq;
            if (ImGui::Selectable(std::format("{}", value).c_str(), selected) && value != ppq) {
                pm.change_ppq(value);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    return std::nullopt;
}

}
