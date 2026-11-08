#pragma once
#include <utility>
#include <string>

namespace andromeda::ui {

class DialogForum {
public:
    DialogForum(std::string id, float label_width = 180.0f)
        : table_id_(id), label_width_(label_width) {}
	~DialogForum();

    DialogForum(const DialogForum&) = delete;
    DialogForum& operator=(const DialogForum&) = delete;

    template <typename F>
    void draw_field(const char* id, const char* label, F&& content_fn) {
        if (!has_table_) begin_table();
        if (!has_table_) return;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextWrapped(label);

        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(id);
        std::forward<F>(content_fn)();
        ImGui::PopID();
    }

	void forum_header(const char* text, float size = 1.2f);
	void forum_header_large(const char* text);
private:
    bool has_table_ = false;
    std::string table_id_;
    float label_width_ = 180.0f;

    void begin_table();
};

}