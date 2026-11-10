#pragma once

#include <vector>
#include <functional>
#include <utility>

namespace andromeda::ui::popups {

struct ContextButton {
	const char* label; // an empty/nullptr label is a separator
	std::function<void()> action;

	template <typename F>
	ContextButton(const char* lbl, F&& act)
		: label(lbl), action(std::forward<F>(act)) { }

	ContextButton(const char* lbl = nullptr) : label(lbl), action(nullptr) {}
};

// abstract class for popup menus
class ContextPopup {
public:
	explicit ContextPopup(const char* id, std::vector<ContextButton> buttons) :
		id_(id), buttons_(std::move(buttons)) {}
	virtual ~ContextPopup() = default;

	virtual void show();
	void draw();
private:
	std::vector<ContextButton> buttons_{};
	const char* id_;
	bool pending_open_ = false;
};

}