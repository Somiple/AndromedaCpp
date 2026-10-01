#pragma once

#include <imgui.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dock_panel.h"

namespace andromeda::app {

	constexpr const char* DOCKSPACE_ID = "MainDockSpace";
	constexpr ImGuiID DOCK_CLASS_ID = 0x41444D4B; // lol random id, just needs to be non-zero
	constexpr ImGuiDockNodeFlags DOCKSPACE_FLAGS =
		ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingOverCentralNode;

	class DockManager {
	public:
		DockManager() = default;
		DockGroup& add_group(std::string id, DockGroupConfig config = {});
		DockGroup* get_group(const std::string& id);
		
		void begin_frame();
		void draw();
		void reset_layout();
		bool central_rect(ImVec2& pos, ImVec2& size) const;
	private:
		std::vector<std::unique_ptr<DockGroup>> groups_;
		ImGuiID dockspace_id_ = 0;
		bool host_ready = false;

		bool layout_built_ = false;
		bool rebuild_pending_ = false;

		void rebuild_imgui_dockspace();
		void draw_panel(DockGroup& group, DockPanel& panel);
		void apply_layout();
	};
}