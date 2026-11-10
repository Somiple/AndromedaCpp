#pragma once

#include "context_popup.h"
#include <memory>

namespace andromeda::ui::popups {

class ContextPopupRegistry {
public:
	ContextPopupRegistry() = default;
	void register_popup(std::unique_ptr<ContextPopup> popup) {
		popup_registry.push_back(std::move(popup));
	}

	template <typename T>
	[[nodiscard]] T* get_popup() {
		for (auto& popup : popup_registry) {
			if (auto* derived = dynamic_cast<T*>(popup.get()))
				return derived;
		}
		return nullptr;
	}

	template <typename F>
	void for_each(F&& action) {
		for (auto& popup : popup_registry) {
			action(popup.get());
		}
	}

private:
	std::vector<std::unique_ptr<ContextPopup>> popup_registry{};
};

}