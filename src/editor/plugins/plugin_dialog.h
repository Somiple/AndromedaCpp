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

#include "ui/widgets/custom_widgets.h"
#include "ui/dialogs/dialog.h"
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

/// Payload for PluginDialog::on_open.
/// refactor: the dialog is a registry singleton now, so the plugin it presents and the track it
/// runs against travel in this payload instead of being pushed through the retired manager.
struct PluginDialogOptions {
    std::shared_ptr<PluginLua> plugin;  ///< plugin to present; may be null
    std::size_t track = 0;              ///< track the plugin runs against
};

class PluginDialog final : public app::Dialog {
public:
    void init(EditorController* controller);

    std::expected<bool, LuaError> load_plugin_dialog(std::shared_ptr<PluginLua> plugin);

    std::expected<void, LuaError> run_plugin();

    [[nodiscard]] const char* get_title() const override { return title_.c_str(); }
    void draw_content() override;

    /// @param payload must hold a PluginDialogOptions; loads the plugin it names.
    void on_open(const app::DialogPayload& payload) override;

    /// Runs the plugin with the values currently entered.
    void on_click_confirm() override;

    [[nodiscard]] const app::DialogActions get_actions() const override {
        return app::DialogActions::OkCancel;
    }

    /// @return true when the loaded plugin declared fields and needs its window shown.
    [[nodiscard]] bool needs_dialog() const noexcept { return needs_dialog_; }

    /// @return the pending error and clears it, for the window to show in the error dialog.
    std::string take_error();

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

    std::string title_ = "Plugin";
    std::string error_;
    bool needs_dialog_ = false;

    EditorController* _controller;
    bool showing_ = false;
};

}
