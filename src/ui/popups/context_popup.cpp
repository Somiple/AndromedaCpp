#include "context_popup.h"
#include <imgui.h>

namespace andromeda::ui::popups {

void ContextPopup::show() {
	pending_open_ = true;
}

void ContextPopup::draw() {
	ImGui::PushID(this);

	if (pending_open_) {
		pending_open_ = false;
		ImGui::OpenPopup(id_);
	}

	if (!ImGui::BeginPopup(id_)) {
		ImGui::PopID();
		return;
	}

	for (const ContextButton& button : buttons_) {
		if (!button.label || button.label[0] == '\0') {
			ImGui::Separator();
			continue;
		}

		if (ImGui::MenuItem(button.label)) {
			if (button.action) button.action();
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::EndPopup();
	ImGui::PopID();
}

}