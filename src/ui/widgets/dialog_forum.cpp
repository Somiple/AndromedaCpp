#include <imgui.h>
#include "dialog_forum.h"

namespace andromeda::ui {

DialogForum::~DialogForum() {
	if (!has_table_) return;

	ImGui::EndTable();
}

void DialogForum::forum_header(const char* text, float size) {
	if (has_table_) ImGui::EndTable();

	ImGui::SetWindowFontScale(size);
	ImGui::TextUnformatted(text);
	ImGui::SetWindowFontScale(1.0f);
	ImGui::Separator();
	ImGui::Spacing();

	if (has_table_) begin_table();
}

void DialogForum::forum_header_large(const char* text) {
	forum_header(text, 1.5f);
}

void DialogForum::begin_table() {
	has_table_ = ImGui::BeginTable(table_id_.c_str(), 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadInnerX);
	if (!has_table_) return;

	ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 180.0f);
	ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
}

}