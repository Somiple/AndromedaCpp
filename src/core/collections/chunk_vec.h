#pragma once

#include <cstdint>

namespace andromeda::util::collections {
// chunk-bassed vector with inline fast path to replace std::vector in scratch space
template<typename T, size_t CHUNK_CAP = 8>
class ChunkVec {
public:
    ~ChunkVec() { clear(); }
    
    void push(const T& val);
    void push(T&& val);
    T pop();

    [[nodiscard]] bool empty() const { return _size == 0; }
    [[nodiscard]] size_t size() const { return _size; }

    void clear();

private:
    struct Chunk {
        T vals[CHUNK_CAP];
        Chunk* next = nullptr;
    };

    Chunk* _head = nullptr;
    Chunk* _tail = nullptr;

    size_t _head_index = 0;
    size_t _tail_index = 0;
    size_t _size = 0;
};

}