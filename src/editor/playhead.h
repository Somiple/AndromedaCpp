#pragma once

#include <memory>

#include "editor/midi_types.h"
#include "util/shared.h"

namespace andromeda::app {
class MainWindow;
}

namespace andromeda::editor {

class Playhead {
public:
    explicit Playhead(app::MainWindow* app)
        : app_(app) { }

    Playhead(app::MainWindow* app, MIDITick start_tick)
        : app_(app) {
        set_start(start_tick);
    }

    void set_start(MIDITick start_tick);

    MIDITick start_tick = 0;
private:
    // setting the playhead will also automatically set the timer start
    app::MainWindow* app_ = nullptr;
};

}
