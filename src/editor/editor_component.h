#pragma once

#include <cstdint>
#include "editor/util.h"
#include "util/math/vector2.h"

namespace andromeda::app {
class MainWindow;
}

namespace andromeda::editor {

class EditorController;

struct ComponentContext {
	// boundinng box of this component
	ViewRect rect{};
};

/// <summary>
/// A base class which holds common behavior for certain editing functionality such as note editing, track editing, etc.
/// </summary>
class EditorComponent {
	using ComponentFlag = uint16_t;
public:
	// TODO: change MainWindow* to be EditorApp*, whenever that class gets implemented
	explicit EditorComponent(app::MainWindow* app, EditorController* controller)
		: _app(app), _controller(controller) { }
	virtual ~EditorComponent() = default;

	virtual void update() {}

	virtual void on_key_down() {}
	virtual void on_mouse_down() {}
	virtual void on_right_mouse_down() {}
	virtual void on_mouse_move() {}
	virtual void on_mouse_up() {}

	[[nodiscard]] virtual bool can_listen_for_events() const { return true; }
	
#pragma region context functions
	// sets the "bounds" of the component
	void set_rect(ViewRect rect) {
		context().rect = rect;
	}
#pragma endregion

#pragma region flags
	void set_flag(ComponentFlag flag, bool value);
	void enable_flag(ComponentFlag flag);
	void disable_flag(ComponentFlag flag);
	[[nodiscard]] bool get_flag(ComponentFlag flag) const;
#pragma endregion
protected:
	app::MainWindow* _app;
	EditorController* _controller;

	ComponentContext& context() { return _context; }
	const ComponentContext& context() const { return _context; }
private:
	ComponentFlag _flags = 0x0;
	ComponentContext _context{};
};

}
