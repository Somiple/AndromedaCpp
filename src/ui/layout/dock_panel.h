#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <string>
#include <functional>
#include <utility>

namespace andromeda::app {
	enum class DockPosition {
		Top,
		Left,
		Right,
		Bottom
	};

	struct DockPanelConfig {
		bool visible = true;
		bool is_flex = false;
		bool dockable = true;

		ImVec2 padding{ 8.0f, 4.0f };
		ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;
	};

	struct DockGroupConfig {
		DockPosition position = DockPosition::Top;

		bool fixed = false;
		bool resizable = true;

		float extent = 200.0f;
		std::function<float()> extent_fn;

		ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;

		float resolved_extent() const { return extent_fn ? extent_fn() : extent; }
	};

	class DockPanel {
	public:
		explicit DockPanel(DockPanelConfig config)
			: config(config), initial_config(config) {}

		virtual ~DockPanel() = default;

		virtual const char* id() const = 0;
		virtual const char* title() const = 0;
		virtual void draw() = 0;

		virtual bool available() const { return true; }

		std::string window_name() const {
			return std::string(title()) + "###" + id();
		}

		void reset() {
			config = initial_config;
		}

		DockPanelConfig config;
		DockPanelConfig initial_config;

		// refactor: the layout-solver bookkeeping below used to be public. only DockManager's
		// solver reads or writes it, so it is private now and driven through these accessors
		// (see docs/ARCHITECTURE.md §11). config / initial_config stay public: they are the
		// panel API.

		[[nodiscard]] bool grip_held() const { return grip_held_; }
		void set_grip_held(bool held) { grip_held_ = held; }

		[[nodiscard]] ImVec2 measured() const { return measured_; }
		void set_measured(ImVec2 size) { measured_ = size; }

		[[nodiscard]] ImGuiID last_node_id() const { return last_node_id_; }
		void set_last_node_id(ImGuiID id) { last_node_id_ = id; }

		[[nodiscard]] bool in_layout() const { return in_layout_; }
		void set_in_layout(bool value) { in_layout_ = value; }

		[[nodiscard]] float thickness() const { return thickness_; }
		void set_thickness(float value) { thickness_ = value; }

		[[nodiscard]] int thickness_axis() const { return thickness_axis_; }
		void set_thickness_axis(int axis) { thickness_axis_ = axis; }

	private:
		// solver bookkeeping, refreshed every frame by DockManager::apply_layout
		bool grip_held_ = false;
		ImVec2 measured_{ 0.0f, 0.0f };   // content size + padding on both axes, from last frame
		ImGuiID last_node_id_ = 0;        // the leaf this panel was in last frame
		bool in_layout_ = false;          // was a docked leaf in layout last frame
		float thickness_ = 0.0f;          // last settled bar thickness...
		int thickness_axis_ = -1;         // ...and the axis it was measured on
	};

	class DockGroup {
	public:
		explicit DockGroup(std::string id, DockGroupConfig config = {})
			: id_(std::move(id)), config(config), initial_config(config) {}

		const char* id() const {
			return id_.c_str();
		}

		ImGuiID dock_id() const {
			return dock_id_;
		}

		void set_dock_id(ImGuiID id) {
			dock_id_ = id;
		}

		DockGroupConfig config;
		DockGroupConfig initial_config;
		std::vector<std::unique_ptr<DockPanel>> panels;

		template<typename T, typename... Args>
		T& add_panel(Args&&... args) {
			auto panel = std::make_unique<T>(std::forward<Args>(args)...);
			T& result = *panel;
			panels.push_back(std::move(panel));
			return result;
		}

		void reset() {
			config = initial_config;
			for (auto& panel : panels) {
				panel->reset();
			}
		}
	private:
		std::string id_;
		ImGuiID dock_id_ = 0;
	};

	class FnDock final : public DockPanel {
	public:
		FnDock(std::string id, std::string title, DockPanelConfig config,
			std::function<void()> draw_fn, std::function<bool()> available_fn = {}
		)
			: DockPanel(std::move(config)),
			id_(std::move(id)),
			title_(std::move(title)),
			draw_fn_(std::move(draw_fn)),
			available_fn_(std::move(available_fn)) {}

		const char* id() const override {
			return id_.c_str();
		}

		const char* title() const override {
			return title_.c_str();
		}

		void draw() override {
			if (draw_fn_) draw_fn_();
		}

		bool available() const override {
			return !available_fn_ || available_fn_();
		}

	private:
		std::string id_;
		std::string title_;

		std::function<void()> draw_fn_;
		std::function<bool()> available_fn_;
	};
}