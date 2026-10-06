#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <sol/sol.hpp>

#include "app/custom_widgets.h"
#include "app/ui/dialog.h"
#include "editor/actions.h"
#include "editor/plugins/plugin_lua.h"

namespace andromeda::editor {

class EditorController;

namespace plugin_field {

struct Label {
    std::string contents;
};
struct Number {
    std::string field_id;
    std::string label;
    app::NumericField<double> field;
};
struct Slider {
    std::string field_id;
    std::string label;
    double value;
    double min;
    double max;
    std::optional<double> step;
};
struct TextField {
    std::string field_id;
    std::string label;
    std::array<char, 256> value{};
};
struct Toggle {
    std::string field_id;
    std::string label;
    bool value;
};
struct Dropdown {
    std::string field_id;
    std::string label;
    std::size_t value;
    std::vector<std::string> value_labels;
};
struct Separator {};

}

using DialogField =
    std::variant<plugin_field::Label, plugin_field::Number, plugin_field::Slider,
                 plugin_field::TextField, plugin_field::Toggle, plugin_field::Dropdown,
                 plugin_field::Separator>;

class PluginDialog final : public app::Dialog {
public:
    void init(EditorController* controller);

    std::expected<bool, LuaError> load_plugin_dialog(std::shared_ptr<PluginLua> plugin);

    std::expected<void, LuaError> run_plugin();

    app::MaybeDlgAction draw(const app::ImageResources& images) override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return app::dialog_names::DIALOG_NAME_PLUGIN_DIALOG;
    }
    [[nodiscard]] std::string get_dialog_title() const override;
    [[nodiscard]] std::optional<app::DialogActionButtons> get_action_buttons() const override;

    std::size_t curr_track = 0;

private:
    void add_separator();
    void add_label(std::string label);
    void add_number(std::string field_id, const sol::table& field_contents);
    void add_slider(std::string field_id, const sol::table& field_contents);
    void add_textedit(std::string field_id, const sol::table& field_contents);
    void add_toggle(std::string field_id, const sol::table& field_contents);
    std::expected<void, LuaError> add_dropdown(std::string field_id,
                                               const sol::table& field_contents);

    [[nodiscard]] sol::optional<sol::table> get_field_by_id(const std::string& id) const;

    template <typename T>
    void write_back(const std::string& field_id, const T& value) const;

    std::shared_ptr<PluginLua> plugin_;
    std::vector<DialogField> fields_;

    EditorController* _controller;
    bool showing_ = false;
};

}
