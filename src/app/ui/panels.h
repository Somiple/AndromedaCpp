#pragma once

namespace andromeda::app {

class MainWindow;

void draw_panel_process_stats(MainWindow& parent);
void draw_panel_editor_tools_toollist(MainWindow& parent);
void draw_panel_editor_tools_note_snap(MainWindow& parent);
void draw_panel_editor_tools_note_properties(MainWindow& parent);
void draw_panel_editor_tools_track_options(MainWindow& parent);
void draw_panel_editor_tools_zoom_controls(MainWindow& parent);
void draw_panel_playback_buttons(MainWindow& parent);
void draw_panel_fancy_playback(MainWindow& parent);
void draw_panel_side_controls(MainWindow& parent);
void draw_panel_playhead_ui(MainWindow& parent);
void draw_panel_scroll_navigation(MainWindow& parent);
void draw_panel_scroll_navigation_vertical(MainWindow& parent);
void draw_panel_data_viewer(MainWindow& parent);
void draw_panel_bar_numbers(MainWindow& parent);
void draw_panel_track_list(MainWindow& parent);

}
