//
// Created by Brian Song on 5/24/26.
//

#ifndef ORDERBEAMER_VECTOR_H
#define ORDERBEAMER_VECTOR_H
#include <algorithm>
#include <concepts>
#include <initializer_list>
#include <memory>
#include <utility>

/* unsafe impl of std::Vector and basic functionality */
template <std::copyable T, int N> requires (N > 0)
class Vector {
    const int sz {N};
    int curr {0};
    const std::unique_ptr<T[]> data {std::make_unique<T[]>(N)};
public:
    // ensure size(startingList) <= N
    explicit Vector(const std::initializer_list<T> startingList = {}) noexcept : curr {static_cast<int>(startingList.size())} {
        std::copy(startingList.begin(), startingList.end(), data.get());
    }
    [[nodiscard]] constexpr int capacity() const noexcept {
        return sz;
    }
    [[nodiscard]] int size() const noexcept {
        return curr;
    }
    // unsafe access
    T& operator[](int index) noexcept {
        return data[index];
    }
    // unsafe access
    void pushBack(const T& copyableNext) noexcept {
        data[curr++] = T{copyableNext};
    }
    // unsafe access
    void pushBack(T&& movableNext) noexcept {
        data[curr++] = std::move(movableNext);
    }
    const T* begin() const noexcept {
        return data.get();
    }
    // unsafe access
    const T* end() const noexcept {
        return data.get() + curr;
    }
};

#endif //ORDERBEAMER_VECTOR_H
