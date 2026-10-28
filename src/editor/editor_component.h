#pragma once

#include <cstdint>
#include <memory>
#include "editor/util.h"
#include "editor/editor_context.h"
#include "core/math/vector2.h"
// Absolute (root-relative) rather than "overlays/overlay_drawer.h": the include
// root is `src`, so this keeps working if EditorComponent ever moves folders.
#include "editor/overlays/overlay_drawer.h"

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
	/// @param context Non-owning narrow context owned by the host; must outlive this component.
	/// @param controller Owning controller; must outlive this component.
	explicit EditorComponent(EditorContext* context, EditorController* controller)
		: _editor_context(context), _controller(controller) { }

#pragma region narrow context accessors
	// REFACTOR: these replace the former `_app->get_...()` reach-through into MainWindow.
	// Each returns exactly the slice the editing passes used, and tolerates a null context so
	// construction order keeps working unchanged.
	[[nodiscard]] const util::math::Vector2<float>* mouse_pos() const noexcept {
		return _editor_context != nullptr ? _editor_context->mouse_pos : nullptr;
	}
	[[nodiscard]] const KeyModifierState* key_modifier_state() const noexcept {
		return _editor_context != nullptr ? _editor_context->key_modifiers : nullptr;
	}
	[[nodiscard]] bool mouse_over_ui() const noexcept {
		return _editor_context != nullptr && _editor_context->mouse_over_ui != nullptr &&
			*_editor_context->mouse_over_ui;
	}
	[[nodiscard]] TickView central_tick_view() const noexcept {
		return _editor_context != nullptr ? _editor_context->central_tick_view : TickView{};
	}
	// REFACTOR: returns the same shared handle MainWindow::navigation() exposed, so existing
	// `navigation()->value` expressions keep working. The handle is owned by the host context.
	[[nodiscard]] util::SharedPtr<PianoRollNavigation>& navigation() const noexcept {
		static util::SharedPtr<PianoRollNavigation> empty;
		return _editor_context != nullptr && _editor_context->nav != nullptr ? *_editor_context->nav
			: empty;
	}
	[[nodiscard]] util::SharedPtr<TrackViewNavigation>& track_navigation() const noexcept {
		static util::SharedPtr<TrackViewNavigation> empty;
		return _editor_context != nullptr && _editor_context->track_nav != nullptr
			? *_editor_context->track_nav
			: empty;
	}
	[[nodiscard]] const ViewSnapshot& view() const noexcept {
		static const ViewSnapshot empty{};
		return _editor_context != nullptr ? _editor_context->view : empty;
	}
	[[nodiscard]] IPlaybackProvider* playback() const noexcept {
		return _editor_context != nullptr ? _editor_context->playback : nullptr;
	}
	[[nodiscard]] EditorContext* editor_context() const noexcept { return _editor_context; }
#pragma endregion
	virtual ~EditorComponent() = default;

	virtual void update() {}

	virtual void on_key_down() {}
	virtual void on_mouse_down() {}
	virtual void on_right_mouse_down() {}
	virtual void on_mouse_move() {}
	virtual void on_mouse_up() {}

	void draw_overlays() const {
		if (_overlay_drawer) _overlay_drawer->draw_overlays(make_overlay_transform());
	}

	[[nodiscard]] virtual bool can_listen_for_events() const { return true; }

	/// Consumes the "this component changed something" latch.
	/// Components call `mark_edited()` whenever they mutate project data; consumers
	/// poll this once per frame to decide whether a timeline rebuild is needed.
	/// REFACTOR: moved down from `MetaEditing`, which was the only owner while every
	/// other editing pass had to do without it (the original TODO asked for exactly this).
	/// @return `true` once per edit burst, then `false` until the next `mark_edited()`.
	[[nodiscard]] bool has_edited_recently() {
		const bool last_edited = _edited_recently;
		_edited_recently = false;
		return last_edited;
	}
	
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

	overlays::OverlayDrawer* overlay_drawer() { return _overlay_drawer.get(); }

	ComponentContext& context() { return _context; }
	const ComponentContext& context() const { return _context; }

	virtual ViewRect get_working_rect() const { return _context.rect; }
protected:
	/// Flags this component as having changed project data since the last poll.
	void mark_edited() { _edited_recently = true; }

	EditorContext* _editor_context = nullptr; ///< Non-owning; injected by the host at construction.
	EditorController* _controller;
	std::unique_ptr<overlays::OverlayDrawer> _overlay_drawer;
private:
	ComponentFlag _flags = 0x0;
	bool _edited_recently = false;
	ComponentContext _context{};

	[[nodiscard]] virtual overlays::TransformFn make_overlay_transform() const {
		return [&](const auto& _) { return std::pair{ util::math::Vector2<float>{}, util::math::Vector2<float>{} }; };
	}
};

}
