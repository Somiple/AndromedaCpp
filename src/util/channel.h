#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace andromeda::util {

// read seq before try_recv, or a notify before the wait is lost
struct Waker {
    std::mutex mutex;
    std::condition_variable cv;
    std::uint64_t seq = 0;

    void notify() {
        {
            std::lock_guard lock(mutex);
            ++seq;
        }
        cv.notify_all();
    }
};

template <typename T>
class Channel {
public:
    explicit Channel(std::size_t capacity) : state_(std::make_shared<State>(capacity)) {}

    bool try_send(T value) const {
        {
            std::lock_guard lock(state_->mutex);
            if (state_->queue.size() >= state_->capacity) {
                return false;
            }
            state_->queue.push_back(std::move(value));
        }
        state_->cv.notify_one();
        if (state_->waker) {
            state_->waker->notify();
        }
        return true;
    }

    std::optional<T> try_recv() const {
        std::lock_guard lock(state_->mutex);
        if (state_->queue.empty()) {
            return std::nullopt;
        }
        T value = std::move(state_->queue.front());
        state_->queue.pop_front();
        return value;
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lock(state_->mutex);
        return state_->queue.empty();
    }

    void set_waker(std::shared_ptr<Waker> waker) const { state_->waker = std::move(waker); }

private:
    struct State {
        explicit State(std::size_t cap) : capacity(cap) {}

        mutable std::mutex mutex;
        std::condition_variable cv;
        std::deque<T> queue;
        std::size_t capacity;
        std::shared_ptr<Waker> waker;
    };

    std::shared_ptr<State> state_;
};

template <typename A, typename B>
struct SelectResult {
    std::optional<A> first;
    bool second_ready = false;
};

template <typename A, typename B>
SelectResult<A, B> select2(const Channel<A>& a, const Channel<B>& b, const Waker& waker,
                           std::chrono::milliseconds timeout) {
    SelectResult<A, B> result;

    std::unique_lock lock(const_cast<Waker&>(waker).mutex);
    const std::uint64_t seen = waker.seq;

    lock.unlock();
    if (auto v = a.try_recv()) {
        result.first = std::move(v);
        return result;
    }
    if (b.try_recv().has_value()) {
        result.second_ready = true;
        return result;
    }
    lock.lock();

    const_cast<Waker&>(waker).cv.wait_for(lock, timeout,
                                          [&] { return waker.seq != seen; });
    lock.unlock();

    if (auto v = a.try_recv()) {
        result.first = std::move(v);
        return result;
    }
    if (b.try_recv().has_value()) {
        result.second_ready = true;
    }
    return result;
}

}
