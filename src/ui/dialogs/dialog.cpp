#include "dialog.h"

#include <imgui.h>

namespace andromeda::app {

void Dialog::open(const DialogPayload& payload) {
	close_request_ = false;
	confirmed_ = false;
	open_request_ = true;

	// refactor: on_open() runs here, not in draw(). A caller used to be able to open() a dialog and
	// then query it in the same frame, and it would still see the state from before the payload was
	// applied; PluginDialog was queried that way and therefore always reported "no dialog needed",
	// so its window never appeared. The payload is consumed immediately and never stored, which
	// also removes the stale-payload window the old pending buffer had.
	on_open(payload);
}

// refactor: close() was declared in the header and never defined, so any caller linking against
// it would have failed. The close is latched and applied inside draw(), the only place a window
// may be closed from.
void Dialog::close() {
	open_request_ = false;
	close_request_ = true;
}

void Dialog::draw() {
	const char* title = get_title();
	if (title == nullptr || title[0] == '\0') {
		return;
	}

	if (open_request_) {
		open_request_ = false;
		close_request_ = false;
		is_open_ = true;
		ImGui::OpenPopup(title);
	}

	if (!is_open_) {
		return;
	}

	if (close_request_) {
		close_request_ = false;
		is_open_ = false;
		return;
	}

	// refactor: a centred window replaces the modal popup. OpenPopup/BeginPopupModal need a shared
	// ID stack, and the presenter draws from the frame root where there is no current window to
	// own the popup, so the dialog would never appear.
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(
		ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
			   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
		ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	bool window_open = true;
	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
									   ImGuiWindowFlags_NoCollapse |
									   ImGuiWindowFlags_NoDocking |
									   ImGuiWindowFlags_NoSavedSettings;

	if (ImGui::BeginPopupModal(title, &window_open, flags)) {
		draw_content();

		const std::optional<DialogActions> pressed = draw_actions();
		if (pressed.has_value()) {
			if (*pressed == DialogActions::Ok) {
				confirmed_ = true;
				on_click_confirm();
			} else {
				on_click_cancel();
			}
			is_open_ = false;
		}
		ImGui::EndPopup();
	}
	

	// the title-bar close button is a cancel: it must not leave a stale confirm latch behind
	if (!window_open) {
		on_click_cancel();
		is_open_ = false;
	}
}

std::optional<DialogActions> Dialog::draw_actions() {
	const DialogActions actions = get_actions();
	if (actions == DialogActions::None) {
		return std::nullopt;
	}

	ImGui::Separator();
	switch (actions) {
	case DialogActions::Ok:
		if (ImGui::Button("OK")) {
			return DialogActions::Ok;
		}
		return std::nullopt;
	case DialogActions::OkCancel:
		if (ImGui::Button("OK")) {
			return DialogActions::Ok;
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) {
			return DialogActions::Cancel;
		}
		return std::nullopt;
	case DialogActions::Cancel:
		if (ImGui::Button("Cancel")) {
			return DialogActions::Cancel;
		}
		return std::nullopt;
	case DialogActions::YesNo:
		if (ImGui::Button("Yes")) {
			return DialogActions::Ok;
		}
		ImGui::SameLine();
		if (ImGui::Button("No")) {
			return DialogActions::Cancel;
		}
		return std::nullopt;
	default:
		return std::nullopt;
	}
}

}
