#include "ui/dialogs/edit_function_params_dialog.h"

#include <imgui.h>

#include <cstdio>
#include <utility>

#include "core/debugger.h"

namespace andromeda::app {

// refactor: replaces the old init_dialog(DialogArgs&) body, which reached into a
// std::vector<std::any> for its argument. The payload is typed now, so a mismatched open is a
// diagnostic instead of an unchecked any_cast.
void EditFunctionParamsDialog::on_open(const DialogPayload& payload) {
    const auto* options = payload.get_if<EditFunctionDialogOptions>();
    if (options == nullptr) {
        util::Debugger::log_error(
            "EditFunctionParamsDialog opened without an EditFunctionDialogOptions payload");
        return;
    }

    m_function_id = options->function_id;
    m_title = options->title;
    m_params = options->params;
    m_values = options->values;
    // make sure every declared parameter has a value, even if the caller passed a partial bag.
    m_values.prefill(m_params);
}

void EditFunctionParamsDialog::draw_content() {
    // one row per declared parameter; the widget kind is driven purely by EditParam::type.
    for (const editor::EditParam& param : m_params) {
        ImGui::PushID(param.id.c_str());

        switch (param.type) {
        case editor::EditParamType::Integer: {
            int value = m_values.get_int(param.id, param.default_int);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::DragInt("##value", &value, 1.0f,
                               param.min_int.value_or(0), param.max_int.value_or(0))) {
                if (param.min_int) value = value < *param.min_int ? *param.min_int : value;
                if (param.max_int) value = value > *param.max_int ? *param.max_int : value;
                m_values.set(param.id, value);
            }
            break;
        }
        case editor::EditParamType::Decimal: {
            // ImGui's DragFloat is float-only while the value bag stores double, so convert
            // once per frame and widen the edited result on write-back.
            float value = static_cast<float>(m_values.get_decimal(param.id, param.default_decimal));
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::DragFloat("##value", &value, 0.01f)) {
                m_values.set(param.id, static_cast<double>(value));
            }
            break;
        }
        case editor::EditParamType::Boolean: {
            bool value = m_values.get_bool(param.id, param.default_bool);
            if (ImGui::Checkbox("##value", &value)) {
                m_values.set(param.id, value);
            }
            break;
        }
        case editor::EditParamType::Text:
        case editor::EditParamType::Choice: {
            const std::string current = m_values.get_text(param.id, param.default_text);

            if (param.type == editor::EditParamType::Choice) {
                int selected = 0;
                for (std::size_t i = 0; i < param.choices.size(); ++i) {
                    if (param.choices[i] == current) {
                        selected = static_cast<int>(i);
                        break;
                    }
                }
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::BeginCombo("##value", param.choices.empty()
                                                     ? ""
                                                     : param.choices[selected].c_str())) {
                    for (std::size_t i = 0; i < param.choices.size(); ++i) {
                        const bool is_selected = (static_cast<int>(i) == selected);
                        if (ImGui::Selectable(param.choices[i].c_str(), is_selected)) {
                            m_values.set(param.id, param.choices[i]);
                        }
                        if (is_selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            } else {
                char buffer[256];
                std::snprintf(buffer, sizeof(buffer), "%s", current.c_str());
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::InputText("##value", buffer, sizeof(buffer))) {
                    m_values.set(param.id, std::string(buffer));
                }
            }
            break;
        }
        }

        ImGui::SameLine();
        ImGui::TextUnformatted(param.label.c_str());
        ImGui::PopID();
    }
}

} // namespace andromeda::app
