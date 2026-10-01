#include "dock_manager.h"
#include "dock_panel.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <string>
#include <algorithm>
#include <unordered_map>

namespace andromeda::app {

namespace {

static float extent_fraction(DockPosition position, float extent, ImVec2 available_size) {
	float available_extent = 0.0f;

	switch (position) {
	case DockPosition::Top:
	case DockPosition::Bottom:
		available_extent = available_size.y;
		break;

	case DockPosition::Left:
	case DockPosition::Right:
		available_extent = available_size.x;
		break;
	}

	if (available_extent <= 0.0f)
		return 0.0f;

	return std::clamp(
		extent / available_extent,
		0.0f,
		1.0f
	);
}

static bool is_horizontal(const DockPosition& position) {
	return position == DockPosition::Top || position == DockPosition::Bottom;
}

static bool contains(const ImGuiDockNode* n, const ImGuiDockNode* target) {
	if (!n || !target) return false;
	if (n == target) return true;
	return !n->IsLeafNode() &&
		(contains(n->ChildNodes[0], target) || contains(n->ChildNodes[1], target));
}

struct BarInfo {
	int axis = -1;                 // axis the bar is stacked against the center on; -1 if none
	ImGuiDockNode* bar = nullptr;  // the topmost node of the bar
};

static BarInfo find_bar(ImGuiDockNode* leaf, const ImGuiDockNode* central) {
	for (ImGuiDockNode* n = leaf; n->ParentNode; n = n->ParentNode) {
		ImGuiDockNode* p = n->ParentNode;
		ImGuiDockNode* sibling = p->ChildNodes[0] == n ? p->ChildNodes[1] : p->ChildNodes[0];
		if (sibling && contains(sibling, central)) return { static_cast<int>(p->SplitAxis), n };
	}
	return {};
}

struct LeafInfo {
	DockPanel* panel = nullptr;
	DockGroup* group = nullptr;
	bool fresh = false;   // the panel just arrived in this node
	bool resumed = false; // was hidden/unavailable last frame
};

struct Pref {
	float fixed = 0.0f;   // pixels this subtree needs
	int flex = 0;         // number of leaves that soak up leftover space
};

class LayoutSolver {
public:
	LayoutSolver(std::unordered_map<ImGuiID, LeafInfo> leaves, const ImGuiDockNode* central, float separator, ImVec2 root_size)
		: leaves_(std::move(leaves)), central_(central), separator_(separator), root_size_(root_size) {
	}

	Pref preferred(ImGuiDockNode* n, int axis) const {
		if (n->IsLeafNode()) return leaf_preferred(n, axis);

		const Pref a = preferred(n->ChildNodes[0], axis);
		const Pref b = preferred(n->ChildNodes[1], axis);
		if (static_cast<int>(n->SplitAxis) == axis)
			return { a.fixed + b.fixed + separator_, a.flex + b.flex };
		return { std::max(a.fixed, b.fixed), std::max(a.flex, b.flex) };
	}

	void assign(ImGuiDockNode* n, ImVec2 size) const {
		n->SizeRef = size;
		if (n->IsLeafNode()) return;

		const int s = static_cast<int>(n->SplitAxis);
		ImGuiDockNode* c0 = n->ChildNodes[0];
		ImGuiDockNode* c1 = n->ChildNodes[1];

		const Pref p0 = preferred(c0, s);
		const Pref p1 = preferred(c1, s);
		const int flex = p0.flex + p1.flex;
		const float leftover = std::max(0.0f, size[s] - separator_ - p0.fixed - p1.fixed);
		const float share = flex > 0 ? leftover / static_cast<float>(flex) : 0.0f;

		float s0 = p0.fixed + static_cast<float>(p0.flex) * share;
		float s1 = p1.fixed + static_cast<float>(p1.flex) * share;
		if (flex == 0) s1 += leftover;   // nothing flexible: the last child absorbs

		ImVec2 a = size, b = size;
		a[s] = std::max(s0, 1.0f);
		b[s] = std::max(s1, 1.0f);
		assign(c0, a);
		assign(c1, b);
	}

private:
	Pref leaf_preferred(ImGuiDockNode* n, int axis) const {
		if (n == central_) return { 0.0f, 1 };

		const auto it = leaves_.find(n->ID);
		if (it == leaves_.end()) return {};   // empty or hidden: collapses

		const DockPanel& panel = *it->second.panel;
		const DockGroup& group = *it->second.group;
		const float measured = panel.measured[axis] > 0.0f ? panel.measured[axis] : n->Size[axis];

		const BarInfo bar = find_bar(n, central_);
		if (bar.axis != axis) {
			// row axis: content-sized, or flexible
			return panel.config.is_flex ? Pref{ 0.0f, 1 } : Pref{ measured, 0 };
		}

		// thickness axis
		const LeafInfo& info = it->second;
		const bool home = is_horizontal(group.config.position) == (axis == 1);
		const float extent = group.config.resolved_extent();
		float size;

		if (group.config.fixed) size = extent;
		else if (info.resumed && panel.thickness_axis == axis && panel.thickness > 1.0f) size = panel.thickness;
		else if (!home && !panel.config.is_flex) size = measured;
		else if (group.config.resizable && !info.fresh) size = n->Size[axis];
		else size = extent;
		if (!group.config.fixed) size = std::min(size, std::max(root_size_[axis] * 0.5f, 1.0f));

		return { size, 0 };
	}

	std::unordered_map<ImGuiID, LeafInfo> leaves_;
	const ImGuiDockNode* central_;
	float separator_;
	ImVec2 root_size_;
};

using LeafMap = std::unordered_map<ImGuiID, LeafInfo>;

static bool side_resizable(const ImGuiDockNode* n, const LeafMap& leaves) {
	if (n->IsLeafNode()) {
		const auto it = leaves.find(n->ID);
		if (it == leaves.end()) return true;   // empty / hidden leaf: doesn't object
		const DockGroupConfig& c = it->second.group->config;
		return c.resizable && !c.fixed;
	}
	return side_resizable(n->ChildNodes[0], leaves) && side_resizable(n->ChildNodes[1], leaves);
}

static void set_resize_lock(ImGuiDockNode* n, bool lock) {
	if (lock) n->LocalFlags |= ImGuiDockNodeFlags_NoResize;
	else      n->LocalFlags &= ~ImGuiDockNodeFlags_NoResize;
	n->MergedFlags = n->SharedFlags | n->LocalFlagsInWindows | n->LocalFlags;
}

// ImGui checks the flags of both nodes a splitter touches, so the lock goes on the children
static void lock_splitters(ImGuiDockNode* n, const ImGuiDockNode* central, const LeafMap& leaves) {
	if (n->IsLeafNode()) return;

	ImGuiDockNode* c0 = n->ChildNodes[0];
	ImGuiDockNode* c1 = n->ChildNodes[1];

	bool lock = true;   // default: the solver owns every size
	const bool c0_has_center = contains(c0, central);
	const bool c1_has_center = contains(c1, central);
	if (c0_has_center != c1_has_center)   // this splitter separates a bar from the center
		lock = !side_resizable(c0_has_center ? c1 : c0, leaves);

	set_resize_lock(c0, lock);
	set_resize_lock(c1, lock);

	lock_splitters(c0, central, leaves);
	lock_splitters(c1, central, leaves);
}

static ImGuiDir edge_dir(DockPosition position) {
	switch (position) {
	case DockPosition::Top:    return ImGuiDir_Up;
	case DockPosition::Bottom: return ImGuiDir_Down;
	case DockPosition::Left:   return ImGuiDir_Left;
	case DockPosition::Right:  return ImGuiDir_Right;
	}
	return ImGuiDir_Up;
}

static const ImGuiWindowClass& dockspace_class() {
	static const ImGuiWindowClass c = [] {
		ImGuiWindowClass cls;
		cls.ClassId = DOCK_CLASS_ID;
		cls.DockingAllowUnclassed = false;
		return cls;
	}();
	return c;
}

static ImGuiDir cross_axis_dir(DockPosition position) {
	return is_horizontal(position) ? ImGuiDir_Left : ImGuiDir_Up;
}

static int cross_axis(DockPosition position) {
	return is_horizontal(position) ? 0 : 1;   // 0 = x, 1 = y (ImVec2 index)
}

// no tab bar, and nothing can be dropped on top of a panel to tab with it
static const ImGuiWindowClass& panel_class(bool locked) {
	static const ImGuiWindowClass free_class = [] {
		ImGuiWindowClass c;
		c.DockNodeFlagsOverrideSet =
			ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingOverMe;
		return c;
		}();
	static const ImGuiWindowClass locked_class = [] {
		ImGuiWindowClass c;
		c.DockNodeFlagsOverrideSet =
			ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingOverMe |
			ImGuiDockNodeFlags_NoUndocking;
		return c;
		}();
	return locked ? locked_class : free_class;
}

static float grip_thickness() {
	return ImGui::GetFontSize() * 0.5f;
}

// double line on a panel's leading edge: left edge in Top/Bottom groups, top edge in Left/Right
// ones. Dragging it undocks the panel.
static void grip_for_panel(DockPanel& panel, bool horizontal_group) {
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	const float t = grip_thickness();
	const ImVec2 pos = window->Pos;
	const ImVec2 size = window->Size;
	const float inset = t * 0.5f;

	ImVec2 min, max;
	if (horizontal_group) {
		min = ImVec2(pos.x, pos.y + inset);
		max = ImVec2(pos.x + t, pos.y + size.y - inset);
	}
	else {
		min = ImVec2(pos.x + inset, pos.y);
		max = ImVec2(pos.x + size.x - inset, pos.y + t);
	}

	const bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min, max, false);
	if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) panel.grip_held = true;
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) panel.grip_held = false;

	if (panel.grip_held)
		ImGui::StartMouseMovingWindowOrNode(window, window->DockNode, true);

	const ImU32 col = ImGui::GetColorU32(
		hovered || panel.grip_held ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	ImDrawList* dl = window->DrawList;
	const float a = t * 0.33f, b = t * 0.67f;

	if (horizontal_group) {
		dl->AddLine(ImVec2(min.x + a, min.y), ImVec2(min.x + a, max.y), col);
		dl->AddLine(ImVec2(min.x + b, min.y), ImVec2(min.x + b, max.y), col);
	}
	else {
		dl->AddLine(ImVec2(min.x, min.y + a), ImVec2(max.x, min.y + a), col);
		dl->AddLine(ImVec2(min.x, min.y + b), ImVec2(max.x, min.y + b), col);
	}
}

// writes SizeRef[axis] on every node under `node`; split nodes get the sum of their children
static float assign_size_refs(
	ImGuiDockNode* node, int axis, const std::unordered_map<ImGuiID, float>& wanted
) {
	if (node->IsLeafNode()) {
		const auto it = wanted.find(node->ID);
		if (it != wanted.end()) node->SizeRef[axis] = it->second;
		return node->SizeRef[axis];
	}

	const float a = assign_size_refs(node->ChildNodes[0], axis, wanted);
	const float b = assign_size_refs(node->ChildNodes[1], axis, wanted);
	node->SizeRef[axis] = a + b;
	return a + b;
}

}

DockGroup& DockManager::add_group(std::string id, DockGroupConfig config) {
	std::unique_ptr<DockGroup> group = std::make_unique<DockGroup>(std::move(id), config);
	DockGroup& result = *group;
	groups_.push_back(std::move(group));
	return result;
}

DockGroup* DockManager::get_group(const std::string& id) {
	for (auto& group : groups_) {
		if (id != group->id()) continue;
		return group.get();
	}

	return nullptr;
}

bool DockManager::central_rect(ImVec2& pos, ImVec2& size) const {
	if (!host_ready) return false;

	const ImGuiDockNode* node = ImGui::DockBuilderGetCentralNode(dockspace_id_);
	if (!node) return false;

	pos = node->Pos;
	size = node->Size;

	return size.x > 0.0f && size.y > 0.0f;
}

void DockManager::begin_frame() {
	const ImGuiViewport* viewport = ImGui::GetMainViewport();

	host_ready = false;
	if (viewport->WorkSize.x <= 0.0f || viewport->WorkSize.y <= 0.0f) return;

	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::SetNextWindowViewport(viewport->ID);

	constexpr ImGuiWindowFlags host_flags =
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking;

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
	ImGui::Begin("##MainDockSpaceWindow", nullptr, host_flags);
	ImGui::PopStyleVar(3);

	dockspace_id_ = ImGui::GetID(DOCKSPACE_ID);
	if (!layout_built_ || rebuild_pending_ || !ImGui::DockBuilderGetNode(dockspace_id_)) {
		rebuild_imgui_dockspace();
		layout_built_ = true;
		rebuild_pending_ = false;
	}

	apply_layout();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));
	ImGui::DockSpace(dockspace_id_, ImVec2(0.0f, 0.0f), DOCKSPACE_FLAGS, &dockspace_class());
	ImGui::PopStyleVar();
	ImGui::End();

	host_ready = true;
}

void DockManager::draw() {
	if (!host_ready) return;

	for (auto& group : groups_) {
		for (auto& panel : group->panels)
			draw_panel(*group, *panel);
	}
}

void DockManager::reset_layout() {
	rebuild_pending_ = true;
}

void DockManager::rebuild_imgui_dockspace() {
	ImGuiID root = dockspace_id_;
	if (root == 0) return;
	
	ImGuiViewport* viewport = ImGui::GetMainViewport();

	ImGui::DockBuilderRemoveNode(root);
	ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
	ImGui::DockBuilderSetNodePos(root, viewport->WorkPos);
	ImGui::DockBuilderSetNodeSize(root, viewport->WorkSize);

	ImGuiID center = root;
	ImVec2 available = viewport->WorkSize;

	for (auto& group : groups_) {
		if (group->panels.empty()) {
			group->set_dock_id(0);
			continue;
		}

		const DockPosition position = group->config.position;
		const float extent = group->config.resolved_extent();
		const float fraction = extent_fraction(position, extent, available);

		ImGuiID node = 0;
		ImGui::DockBuilderSplitNode(center, edge_dir(position), fraction, &node, &center);
		group->set_dock_id(node);

		(is_horizontal(position) ? available.y : available.x) -= extent;
	}

	for (auto& group : groups_) {
		const ImGuiID group_node = group->dock_id();
		const size_t count = group->panels.size();
		if (group_node == 0 || count == 0) continue;

		const bool horizontal = is_horizontal(group->config.position);

		ImGuiID remaining = group_node;
		for (size_t i = 0; i < count; ++i) {
			ImGuiID slot = remaining;
			if (i + 1 < count) {
				ImGui::DockBuilderSplitNode(
					remaining,
					cross_axis_dir(group->config.position),
					1.0f / static_cast<float>(count - i),
					&slot,
					&remaining
				);
			}

			group->panels[i]->last_node_id = slot;
			ImGui::DockBuilderDockWindow(group->panels[i]->window_name().c_str(), slot);
		}

		if (group->config.fixed) {
			if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(group_node)) {
				node->LocalFlags |= ImGuiDockNodeFlags_NoResize;
				node->LocalFlags |= horizontal ? ImGuiDockNodeFlags_NoResizeY
					: ImGuiDockNodeFlags_NoResizeX;
			}
		}
	}
	
	ImGui::DockBuilderFinish(root);
}

void DockManager::draw_panel(DockGroup& group, DockPanel& panel) {
	if (!panel.config.visible || !panel.available()) {
		panel.grip_held = false;
		panel.was_active = false;
		return;
	}
	panel.was_active = true;

	// orientation comes from where the panel is now, not from its home group
	const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace_id_);
	const ImGuiWindow* existing = ImGui::FindWindowByName(panel.window_name().c_str());
	const int thick = (existing && existing->DockNode) ? find_bar(existing->DockNode, central).axis : -1;
	const int row_axis = thick >= 0 ? 1 - thick : cross_axis(group.config.position);
	const bool horizontal = row_axis == 0;

	const bool locked = group.config.fixed || !panel.config.dockable;

	ImGuiWindowFlags flags = group.config.window_flags | panel.config.window_flags;
	if (group.config.fixed) flags |= ImGuiWindowFlags_NoMove;
	if (!panel.config.is_flex)
		flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

	// node flags live on the window class, so they follow the panel to any node it lands in
	ImGuiWindowClass cls;
	cls.ClassId = DOCK_CLASS_ID;
	cls.DockingAllowUnclassed = false;
	cls.DockNodeFlagsOverrideSet =
		ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingOverMe |
		(horizontal ? ImGuiDockNodeFlags_NoResizeX : ImGuiDockNodeFlags_NoResizeY);
	if (group.config.fixed)
		cls.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoResizeX | ImGuiDockNodeFlags_NoResizeY;
	if (locked)
		cls.DockNodeFlagsOverrideSet |= ImGuiDockNodeFlags_NoUndocking;
	ImGui::SetNextWindowClass(&cls);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel.config.padding);
	const bool open = ImGui::Begin(panel.window_name().c_str(), nullptr, flags);
	ImGui::PopStyleVar();

	if (open) {
		const bool show_grip = !locked && ImGui::IsWindowDocked();
		const float grip = grip_thickness();

		if (show_grip) {
			if (horizontal) ImGui::Indent(grip);
			else ImGui::SetCursorPosY(ImGui::GetCursorPosY() + grip);
		}
		else {
			panel.grip_held = false;
		}

		panel.draw();

		const ImGuiWindow* window = ImGui::GetCurrentWindow();
		panel.measured = ImVec2(
			window->DC.CursorMaxPos.x - window->DC.CursorStartPos.x + window->WindowPadding.x * 2.0f,
			window->DC.CursorMaxPos.y - window->DC.CursorStartPos.y + window->WindowPadding.y * 2.0f);

		if (show_grip) {
			if (horizontal) ImGui::Unindent(grip);
			grip_for_panel(panel, horizontal);
		}
	}

	ImGui::End();
}

void DockManager::apply_layout() {
	ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspace_id_);
	if (!root || root->IsLeafNode()) return;

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspace_id_);

	std::unordered_map<ImGuiID, LeafInfo> leaves;
	for (auto& group : groups_) {
		for (auto& panel : group->panels) {
			ImGuiDockNode* node = nullptr;

			if (panel->config.visible && panel->available()) {
				const ImGuiWindow* window = ImGui::FindWindowByName(panel->window_name().c_str());
				if (!window) node = ImGui::DockBuilderGetNode(panel->last_node_id);
				else if (window->DockNode) node = window->DockNode;
				else if (window->DockId) node = ImGui::DockBuilderGetNode(window->DockId);
			}

			if (!node || !node->IsLeafNode() || !node->ParentNode) {   // hidden, or floating
				panel->in_layout = false;
				continue;
			}

			const bool fresh = panel->last_node_id != node->ID;
			const bool resumed = !panel->in_layout;   // was hidden / floating until this frame
			const BarInfo info = find_bar(node, central);

			// remember the settled thickness while the node is healthy
			if (!fresh && !resumed && info.axis >= 0 && node->Size[info.axis] > 1.0f) {
				panel->thickness = node->Size[info.axis];
				panel->thickness_axis = info.axis;
			}

			leaves[node->ID] = { panel.get(), group.get(), fresh, resumed };
			panel->last_node_id = node->ID;
			panel->in_layout = true;

			// keep fixed bars from scaling with the window
			if (group->config.fixed && info.bar && info.axis >= 0) {
				info.bar->Size[info.axis] = group->config.resolved_extent();
				info.bar->WantLockSizeOnce = true;
			}
		}
	}

	lock_splitters(root, central, leaves);

	ImGuiContext& g = *ImGui::GetCurrentContext();
	if (g.ActiveId != 0 && g.ActiveIdWindow == g.CurrentWindow) return;

	const LayoutSolver solver(std::move(leaves), central, ImGui::GetStyle().DockingSeparatorSize, viewport->WorkSize);
	solver.assign(root, viewport->WorkSize);
}

}