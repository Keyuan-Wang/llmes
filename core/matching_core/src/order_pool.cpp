/**
 * @file order_pool.cpp
 * @brief Implementation of the Order object pool.
 */

#include "matching/order_pool.hpp"
#include "matching/types.hpp"

namespace matching {

OrderPool::OrderPool(std::size_t capacity) : pool_(capacity) {
    // Build a singly-linked free list through the entire pool.
    // Each slot's next-pointer points to the following slot, forming
    // a stack.  The last slot's next is nullptr (free_head_ init).
    for (auto& o : pool_) {
        o.next = free_head_;
        free_head_ = &o;
    }
}

Order* OrderPool::acquire() {
    if (!free_head_) return nullptr;  // Pool exhausted.

    // Pop the top of the free stack.
    Order* result = free_head_;
    free_head_ = result->next;

    // Clear stale pointers from the slot's previous lifetime.
    result->prev = nullptr;
    result->next = nullptr;

    return result;
}

void OrderPool::release(Order* o) {
    // Push the slot back onto the free stack for reuse.
    o->next = free_head_;
    free_head_ = o;
}

std::size_t OrderPool::capacity() const noexcept {
    return pool_.size();
}

}  // namespace matching
