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
	const auto [tl, br] = to_screen(bounds());

	// placeholder colors, match them to your theme
	ImU32 outline = IM_COL32(255, 255, 255, 255);
	ImU32 fill = 0;   // Default: outline only
	switch (info.type) {
	case SelectionType::Default: break;
	case SelectionType::AddSelect:    outline = IM_COL32(120, 220, 140, 255); fill = IM_COL32(120, 220, 140, 40); break;
	case SelectionType::RemoveSelect: outline = IM_COL32(240, 170, 80, 255);  fill = IM_COL32(240, 170, 80, 40);  break;
	case SelectionType::Delete:       outline = IM_COL32(235, 90, 90, 255);   fill = IM_COL32(235, 90, 90, 60);   break;
	}

	ImDrawList* dl = ImGui::GetForegroundDrawList();   // swap for whatever your renderers draw into
	if (fill) dl->AddRectFilled({tl.x, tl.y}, {br.x, br.y}, fill);
	dl->AddRect({tl.x, tl.y}, {br.x, br.y}, outline);
}

}