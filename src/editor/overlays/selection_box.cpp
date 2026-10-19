// selection_box.cpp (new)
#include "selection_box.h"
#include <algorithm>
#include "imgui.h"

namespace andromeda::editor::overlays {

OverlayRect SelectionBox::bounds() const {
	const int64_t x1 = info.start.x + info.offset.x, x2 = info.end.x + info.offset.x;
	const int64_t y1 = info.start.y + info.offset.y, y2 = info.end.y + info.offset.y;
	return { Vector2<int64_t>(std::min(x1, x2), std::min(y1, y2)),
	         Vector2<int64_t>(std::max(x1, x2), std::max(y1, y2)) };
}

void SelectionBox::draw(const TransformFn& to_screen) {
	const auto [a, b] = to_screen(bounds());

	// the key axis is flipped on screen, so normalize the corners
	const Vector2<float> tl(std::min(a.x, b.x), std::min(a.y, b.y));
	const Vector2<float> br(std::max(a.x, b.x), std::max(a.y, b.y));

	// omit drawing, because they're too small.
	if (std::fabs(tl.x - br.x) <= 0.001f ||
		std::fabs(tl.y - br.y) <= 0.001f) return;

	ImU32 outline = IM_COL32(255, 255, 255, 255);
	ImU32 fill = 0;   // Default: outline only

	const auto& select_color = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
	const auto& select_brighter = ImVec4(select_color.x + 0.2f, select_color.y + 0.2f, select_color.z + 0.2f, 1.0f);

	switch (info.type) {
	case SelectionType::Default: break;
	case SelectionType::AddSelect: {
		outline = ImGui::GetColorU32(select_brighter);
		fill = ImGui::GetColorU32(ImGui::GetColorU32(select_color), 0.3f);
		break;
	}
	case SelectionType::RemoveSelect: {
		outline = IM_COL32(240, 170, 80, 255);
		fill = IM_COL32(240, 170, 80, 40);
		break;
	}
	case SelectionType::Delete: {
		outline = IM_COL32(235, 90, 90, 255); 
		fill = IM_COL32(235, 90, 90, 60); 
		break;
	}
	}

	ImDrawList* dl = ImGui::GetForegroundDrawList(); 
	if (fill) dl->AddRectFilled({tl.x, tl.y}, {br.x, br.y}, fill);
	dl->AddRect({tl.x, tl.y}, {br.x, br.y}, outline);
}

}