#include "editor/plugins/plugin_dialog.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

#include <imgui.h>

#include "editor/editing/lua_note_editing.h"
#include "editor/editing/note_editing.h"
#include "editor/editor_controller.h"
#include "util/debugger.h"

namespace andromeda::editor {

using util::Debugger;

namespace {

std::string table_string(const sol::table& table, const char* key, std::string fallback = "") {
    const sol::optional<std::string> value = table[key];
    return value ? *value : std::move(fallback);
}

}

void PluginDialog::init(EditorController* controller) {
    _controller = controller;
}

std::expected<bool, LuaError> PluginDialog::load_plugin_dialog(std::shared_ptr<PluginLua> plugin) {
    plugin_ = std::move(plugin);

    fields_.clear();

    if (!plugin_->has_dialog_fields()) {
        return false;
    }

    const sol::table fields = plugin_->dialog_field_table();
    if (fields.size() == 0) {
        return false;
    }

    for (std::size_t idx = 1; idx <= fields.size(); ++idx) {
        const sol::object field = fields[idx];
        if (!field.is<sol::table>()) {
            Debugger::log_warning(
                std::format("[PluginWarning] skipping field {} because it is not a table", idx));
            continue;
        }

        const sol::table field_table = field.as<sol::table>();

        if (field_table.empty()) {
            add_separator();
            continue;
        }

        const sol::optional<std::string> field_id_opt = field_table["id"];
        if (!field_id_opt) {
            const sol::optional<std::string> field_type = field_table["type"];
            if (!field_type) {
                return std::unexpected("[PluginError] field has neither an id nor a type");
            }
            if (*field_type != "label") {
                return std::unexpected(std::format(
                    "[PluginError] expected unnested field type to be label, not {}", *field_type));
            }
            add_label(table_string(field_table, "label"));
            continue;
        }

        const std::string field_id = *field_id_opt;

        const sol::optional<sol::table> field_contents_opt = field_table[1];
        if (!field_contents_opt) {
            Debugger::log_warning(std::format(
                "[PluginWarning] skipping field {} because the contents are empty", field_id));
            continue;
        }
        const sol::table field_contents = *field_contents_opt;

        const sol::optional<std::string> field_type = field_contents["type"];
        if (!field_type) {
            return std::unexpected(std::format("[PluginError] field {} has no type", field_id));
        }

        if (*field_type == "separator") {
            add_separator();
        } else if (*field_type == "label") {
            add_label(table_string(field_contents, "label"));
        } else if (*field_type == "number") {
            add_number(field_id, field_contents);
        } else if (*field_type == "slider") {
            add_slider(field_id, field_contents);
        } else if (*field_type == "textedit") {
            add_textedit(field_id, field_contents);
        } else if (*field_type == "toggle") {
            add_toggle(field_id, field_contents);
        } else if (*field_type == "dropdown") {
            if (auto result = add_dropdown(field_id, field_contents); !result) {
                return std::unexpected(result.error());
            }
        } else {
            Debugger::log_warning(std::format(
                "[PluginWarning] Unknown field type \"{}\", skipping...", *field_type));
            continue;
        }
    }

    return true;
}

std::expected<void, LuaError> PluginDialog::run_plugin() {
    if (!plugin_) {
        return {};
    }

    const std::shared_ptr<sol::state> lua = plugin_->lua();
    const sol::protected_function apply_fn = plugin_->on_apply_fn();
    if (!apply_fn.valid()) {
        return {};
    }

    LuaNoteEditing lua_note_editing(_controller);
    // LuaNoteEditing::register_types(*lua);
    (*lua)["curr_track"] = curr_track;
    const sol::protected_function_result result = apply_fn(&lua_note_editing);
    if (!result.valid()) {
        const sol::error err = result;
        Debugger::log_error(
            std::format("[PluginError] (While running {}): \n{}", plugin_->plugin_name, err.what()));
        return std::unexpected(err.what());
    }

    EditorActions* actions = _controller->get_actions();
    if (actions) {
        lua_note_editing.apply_changes(*actions);
    }
    return {};
}

void PluginDialog::add_separator() { fields_.push_back(plugin_field::Separator{}); }

void PluginDialog::add_label(std::string label) {
    fields_.push_back(plugin_field::Label{std::move(label)});
}

void PluginDialog::add_number(std::string field_id, const sol::table& field_contents) {
    const std::string label = table_string(field_contents, "label");
    const sol::optional<double> value = field_contents["value"];

    std::optional<double> min;
    std::optional<double> max;
    if (const sol::optional<sol::table> number_range = field_contents["range"]) {
        if (const sol::optional<double> v = (*number_range)["min"]) {
            min = *v;
        }
        if (const sol::optional<double> v = (*number_range)["max"]) {
            max = *v;
        }
    }

    fields_.push_back(plugin_field::Number{
        std::move(field_id), label, app::NumericField<double>(value.value_or(0.0), min, max)});
}

void PluginDialog::add_slider(std::string field_id, const sol::table& field_contents) {
    const std::string label = table_string(field_contents, "label");
    const sol::optional<double> value = field_contents["value"];

    double min = 0.0;
    double max = 1.0;
    if (const sol::optional<sol::table> slider_range = field_contents["range"]) {
        min = (*slider_range)["min"].get_or(0.0);
        max = (*slider_range)["max"].get_or(1.0);
    }

    std::optional<double> step;
    if (const sol::optional<double> s = field_contents["step"]) {
        step = *s;
    }

    fields_.push_back(
        plugin_field::Slider{std::move(field_id), label, value.value_or(0.0), min, max, step});
}

void PluginDialog::add_textedit(std::string field_id, const sol::table& field_contents) {
    const std::string label = table_string(field_contents, "label");
    const std::string value = table_string(field_contents, "value");

    plugin_field::TextField field{std::move(field_id), label, {}};
    const std::size_t n = std::min(value.size(), field.value.size() - 1);
    std::memcpy(field.value.data(), value.data(), n);
    field.value[n] = '\0';

    fields_.push_back(std::move(field));
}

void PluginDialog::add_toggle(std::string field_id, const sol::table& field_contents) {
    const std::string label = table_string(field_contents, "label");
    const sol::optional<bool> value = field_contents["value"];

    fields_.push_back(plugin_field::Toggle{std::move(field_id), label, value.value_or(false)});
}

std::expected<void, LuaError> PluginDialog::add_dropdown(std::string field_id,
                                                         const sol::table& field_contents) {
    const std::string label = table_string(field_contents, "label");
    const sol::optional<std::size_t> value_opt = field_contents["value"];
    std::size_t value = value_opt.value_or(0);

    std::vector<std::string> value_labels;

    const sol::optional<sol::table> val_labels = field_contents["value_labels"];
    if (!val_labels) {
        return std::unexpected("Dropdown widget must contain at least one value");
    }

    const std::size_t val_labels_len = val_labels->size();
    if (val_labels_len == 0) {
        return std::unexpected("Dropdown widget must contain at least one value");
    }
    if (value > val_labels_len) {
        value = val_labels_len - 1;
    }

    for (std::size_t i = 1; i <= val_labels_len; ++i) {
        const sol::object label_obj = (*val_labels)[i];
        value_labels.push_back(label_obj.as<std::string>());
    }

    if (value >= value_labels.size()) {
        value = value_labels.size() - 1;
    }

    fields_.push_back(
        plugin_field::Dropdown{std::move(field_id), label, value, std::move(value_labels)});
    return {};
}

sol::optional<sol::table> PluginDialog::get_field_by_id(const std::string& id) const {
    if (!plugin_ || !plugin_->has_dialog_fields()) {
        return sol::nullopt;
    }

    const sol::table dialog_fields = plugin_->dialog_field_table();
    for (std::size_t i = 1; i <= dialog_fields.size(); ++i) {
        const sol::object entry = dialog_fields[i];
        if (!entry.is<sol::table>()) {
            continue;
        }
        const sol::table field = entry.as<sol::table>();
        const sol::optional<std::string> id_ = field["id"];
        if (id_ && *id_ == id) {
            return field;
        }
    }

    return sol::nullopt;
}

template <typename T>
void PluginDialog::write_back(const std::string& field_id, const T& value) const {
    sol::optional<sol::table> field = get_field_by_id(field_id);
    if (!field) {
        return;
    }
    sol::table field_table = *field;
    sol::optional<sol::table> contents = field_table[1];
    if (!contents) {
        return;
    }
    sol::table contents_table = *contents;
    contents_table["value"] = value;
}

app::MaybeDlgAction PluginDialog::draw(const app::ImageResources&) {
    if (plugin_ && plugin_->plugin_info && plugin_->plugin_info->description) {
        ImGui::TextWrapped("%s", plugin_->plugin_info->description->c_str());
        ImGui::Separator();
    }

    for (DialogField& field : fields_) {
        std::visit(
            [this](auto& f) {
                using T = std::decay_t<decltype(f)>;

                if constexpr (std::is_same_v<T, plugin_field::Separator>) {
                    ImGui::Separator();
                } else if constexpr (std::is_same_v<T, plugin_field::Label>) {
                    ImGui::TextUnformatted(f.contents.c_str());
                } else if constexpr (std::is_same_v<T, plugin_field::Number>) {
                    f.field.show(f.label.c_str());
                    if (f.field.has_changed()) {
                        write_back(f.field_id, f.field.value());
                    }
                } else if constexpr (std::is_same_v<T, plugin_field::Slider>) {
                    ImGui::TextUnformatted(f.label.c_str());
                    ImGui::SameLine();
                    ImGui::PushID(f.field_id.c_str());
                    if (ImGui::SliderScalar("##slider", ImGuiDataType_Double, &f.value, &f.min,
                                            &f.max)) {
                        if (f.step && *f.step > 0.0) {
                            f.value = f.min + std::round((f.value - f.min) / *f.step) * *f.step;
                            f.value = std::clamp(f.value, f.min, f.max);
                        }
                        write_back(f.field_id, f.value);
                    }
                    ImGui::PopID();
                } else if constexpr (std::is_same_v<T, plugin_field::TextField>) {
                    ImGui::TextUnformatted(f.label.c_str());
                    ImGui::SameLine();
                    ImGui::PushID(f.field_id.c_str());
                    if (ImGui::InputText("##text", f.value.data(), f.value.size())) {
                        write_back(f.field_id, std::string(f.value.data()));
                    }
                    ImGui::PopID();
                } else if constexpr (std::is_same_v<T, plugin_field::Toggle>) {
                    ImGui::TextUnformatted(f.label.c_str());
                    ImGui::SameLine();
                    ImGui::PushID(f.field_id.c_str());
                    if (ImGui::Checkbox("##toggle", &f.value)) {
                        write_back(f.field_id, f.value);
                    }
                    ImGui::PopID();
                } else {
                    static_assert(std::is_same_v<T, plugin_field::Dropdown>);
                    ImGui::TextUnformatted(f.label.c_str());
                    ImGui::SameLine();
                    ImGui::PushID(f.field_id.c_str());
                    if (!f.value_labels.empty()) {
                        if (ImGui::BeginCombo("##dropdown", f.value_labels[f.value].c_str())) {
                            for (std::size_t i = 0; i < f.value_labels.size(); ++i) {
                                const bool selected = i == f.value;
                                if (ImGui::Selectable(f.value_labels[i].c_str(), selected)) {
                                    f.value = i;
                                    write_back(f.field_id, f.value);
                                }
                                if (selected) {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                    ImGui::PopID();
                }
            },
            field);
    }

    return std::nullopt;
}

std::string PluginDialog::get_dialog_title() const {
    return plugin_ ? plugin_->plugin_name : "Plugin";
}

std::optional<app::DialogActionButtons> PluginDialog::get_action_buttons() const {
    return app::DlgApplyClose{
        [](app::Dialog& dlg) -> app::MaybeDlgAction {
            auto& self = static_cast<PluginDialog&>(dlg);
            if (auto result = self.run_plugin(); result) {
                return app::DialogClose{self.get_dialog_name()};
            } else {
                app::DialogArgs args;
                args.emplace_back(self.plugin_ ? self.plugin_->plugin_name : std::string("Plugin"));
                args.emplace_back(result.error());
                return app::DialogOpen{app::dialog_names::DIALOG_NAME_PLUGIN_ERROR_DIALOG,
                                       std::move(args)};
            }
        },
        app::dialog_default_close_action()};
}

}
