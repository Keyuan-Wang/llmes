#pragma once

#include <vector>

#include "types.hpp"

namespace matching {

class OrderPool {
private:
    std::vector<Order> pool_;
    Order* free_head_ = nullptr;

public:
    explicit OrderPool(std::size_t capacity);

    [[nodiscard]] Order*  acquire();          // Get an empty slot from the top of stack, if return nullptr, the pool is full
    void    release(Order* o);    // return a freed slot back to top of stack

    [[nodiscard]] std::size_t capacity() const noexcept;
};

}