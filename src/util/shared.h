#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace andromeda::util {

template <typename T>
struct Shared {
    mutable std::shared_mutex mutex;
    T value{};

    Shared() = default;
    explicit Shared(T v) : value(std::move(v)) {}
};

template <typename T>
using SharedPtr = std::shared_ptr<Shared<T>>;

template <typename T, typename... Args>
SharedPtr<T> make_shared_rw(Args&&... args) {
    return std::make_shared<Shared<T>>(T(std::forward<Args>(args)...));
}

template <typename T>
struct SharedMut {
    mutable std::mutex mutex;
    T value{};

    SharedMut() = default;
    explicit SharedMut(T v) : value(std::move(v)) {}
};

template <typename T>
using SharedMutPtr = std::shared_ptr<SharedMut<T>>;

template <typename T, typename... Args>
SharedMutPtr<T> make_shared_mut(Args&&... args) {
    return std::make_shared<SharedMut<T>>(T(std::forward<Args>(args)...));
}

}
