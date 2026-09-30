#pragma once

#include <vector>

#include "midi/events/meta_event.h"

namespace andromeda::editor {

std::vector<midi::MetaEvent> merge_metas(std::vector<midi::MetaEvent> metas_1,
                                         std::vector<midi::MetaEvent> metas_2);

}
