#include "editor_component.h"
#include "app/main_window.h"
#include "editor_controller.h"

namespace andromeda::editor {

void EditorComponent::set_flag(ComponentFlag flag, bool value) {
	_flags = (_flags & ~flag) | (value ? flag : 0);
}

void EditorComponent::enable_flag(ComponentFlag flag) {
	_flags |= flag;
}

void EditorComponent::disable_flag(ComponentFlag flag) {
	_flags &= ~flag;
}

bool EditorComponent::get_flag(ComponentFlag flag) const {
	return (_flags & flag) != 0;
}

}