#include "chunk_vec.h"
#include <utility>

namespace andromeda::util::collections {

#define CHUNK_VEC_FUNC(type) \
	template <typename T, std::size_t CHUNK_CAP> \
	type ChunkVec<T, CHUNK_CAP>

CHUNK_VEC_FUNC(void)::push(const T& val) {
	if (!_tail || _tail_index == CHUNK_CAP) {
		Chunk* chunk = new Chunk;

		if (_tail) _tail->next = chunk;
		else _head = chunk;

		_tail = chunk;
		_tail_index = 0;
	}

	_tail->vals[_tail_index++] = val;
	++_size;
}

CHUNK_VEC_FUNC(void)::push(T&& val) {
	if (!_tail || _tail_index == CHUNK_CAP) {
		Chunk* chunk = new Chunk;

		if (_tail) _tail->next = chunk;
		else _head = chunk;

		_tail = chunk;
		_tail_index = 0;
	}

	_tail->vals[_tail_index++] = std::move(val);
	++_size;
}

CHUNK_VEC_FUNC(T)::pop() {
	T val = std::move(_head->vals[_head_index++]);
	--_size;

	if (_head == _tail) {
		// case with final chunk
		if (_head_index == _tail_index) {
			delete _head;
			_head = nullptr;
			_tail = nullptr;
			_head_index = 0;
			_tail_index = 0;
		}
	} else if (_head_index == CHUNK_CAP) {
		// finished consuming a non-tail chunk
		Chunk* old = _head;
		_head = _head->next;
		delete old;
		_head_index = 0;
	}

	return val;
}

CHUNK_VEC_FUNC(void)::clear() {
	while (_head) {
		Chunk* next = _head->next;
		delete _head;
		_head = next;
	}

	_tail = nullptr;
	_head_index = 0;
	_tail_index = 0;
	_size = 0;
}

#undef CHUNK_VEC_FUNC

// instantiate what we only need
// for note ids
template class ChunkVec<std::uint32_t, 256>;

}