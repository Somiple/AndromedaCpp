#include "editor/editing/meta_editing/meta_sequence_funcs.h"

#include <utility>

namespace andromeda::editor {

std::vector<midi::MetaEvent> merge_metas(std::vector<midi::MetaEvent> metas_1,
                                         std::vector<midi::MetaEvent> metas_2) {
    std::vector<midi::MetaEvent> merged;
    merged.reserve(metas_1.size() + metas_2.size());

    std::size_t i1 = 0;
    std::size_t i2 = 0;

    for (;;) {
        if (i1 < metas_1.size() && i2 < metas_2.size()) {
            if (metas_1[i1].tick <= metas_2[i2].tick) {
                merged.push_back(std::move(metas_1[i1++]));
            } else {
                merged.push_back(std::move(metas_2[i2++]));
            }
        } else if (i1 < metas_1.size()) {
            merged.insert(merged.end(),
                          std::make_move_iterator(metas_1.begin() + static_cast<std::ptrdiff_t>(i1)),
                          std::make_move_iterator(metas_1.end()));
            break;
        } else if (i2 < metas_2.size()) {
            merged.insert(merged.end(),
                          std::make_move_iterator(metas_2.begin() + static_cast<std::ptrdiff_t>(i2)),
                          std::make_move_iterator(metas_2.end()));
            break;
        } else {
            break;
        }
    }

    return merged;
}

}
