#include <imgui.h>
#include <string>
#include <format>
#include <algorithm>

#include "track_properties.h"
#include "ui/widgets/dialog_forum.h"

namespace {
constexpr auto imcol_to_array = [](const ImColor& col) {
    const auto convert = [](float value) -> std::uint8_t {
        return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        };

    return std::array<std::uint8_t, 3>{
        convert(col.Value.x),
            convert(col.Value.y),
            convert(col.Value.z)
    };
};

}

namespace andromeda::app {

void TrackPropertiesDialog::on_open(const DialogPayload& payload) {
	const auto* trk = payload.get_if<midi::MIDITrack*>();
	if (trk == nullptr) return;
	this->track = *trk;
}

const char* TrackPropertiesDialog::get_title() const {
    return "Track properties";
}

void TrackPropertiesDialog::draw_content() {
    if (track == nullptr) return;

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
	ImGui::BeginChild("##track_properties_content", ImVec2(400.0f, 0.0f),
		ImGuiChildFlags_AutoResizeY);
	
    {
        ui::DialogForum content("##track_properties_content", 100.0f);
        content.draw_field("##track_name", "Track name", [&]() {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", track->name.has_value() ? track->name->data() : "\0");
            if (ImGui::InputText("", buf, sizeof(buf))) {
                if (buf[0] == '\0')
                    track->name = std::nullopt;
                else
                    track->name = buf;
            }
        });

        content.draw_field("##track_port", "Track port", [&]() {
            int port = track->port;
            if (ImGui::InputInt("", &port, 1)) {
                if (port > 255) port = 255;
                if (port < 0) port = 0;
                track->port = port;
            }
            });

        content.draw_field("##track_color_coded", "Color coded", [&]() {
            bool color_coded = track->project_meta.color_coded;
            if (ImGui::Checkbox("", &color_coded)) {
                track->project_meta.color_coded = color_coded;
                if (color_coded)
                    track->set_color_code(std::move(imcol_to_array(color_code_)));
                else
                    track->clear_color_code();
            }

            ImGui::BeginDisabled(!track->project_meta.color_coded);
            if (ImGui::ColorEdit3("##edit", &color_code_.Value.x)) {
                if (track->project_meta.color_coded)
                    track->set_color_code(std::move(imcol_to_array(color_code_)));
            }
            ImGui::EndDisabled();
            });
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

}