/**
 * @file order_pool.hpp
 * @brief Lock-free-ish object pool for Order reuse.
 *
 * Pre-allocates a contiguous block of Order objects and maintains a
 * singly-linked free list (stack LIFO) over them.  acquire() / release()
 * are O(1) and allocation-free — all memory is reserved at construction.
 *
 * This eliminates malloc/free calls in the hot path of every
 * add_limit_order / cancel_order operation.
 */

#pragma once

#include <vector>

#include "types.hpp"

namespace matching {

class OrderPool {
private:
    std::vector<Order> pool_;     ///< Backing contiguous storage.
    Order* free_head_ = nullptr;  ///< Top of the free-stack.

public:
    /** @brief Pre-allocate @p capacity Orders and link them into the free list. */
    explicit OrderPool(std::size_t capacity);

    /**
     * @brief Pop a slot from the free stack.
     * @return Pointer to a recycled Order, or nullptr if the pool is exhausted.
     */
    Order* acquire();

    /**
     * @brief Push a previously-acquired slot back onto the free stack.
     * @param o  Must have been obtained from acquire() and not already released.
     */
    void release(Order* o);

    /** @brief Total number of pre-allocated slots (fixed at construction). */
    [[nodiscard]] std::size_t capacity() const noexcept;
};

}  // namespace matching
