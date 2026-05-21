/**
 * @file intrusive_list.hpp
 * @brief Intrusive doubly-linked list for price-level order storage.
 *
 * Orders (matching::Order) embed prev/next pointers directly, so
 * push/erase are pointer-only — no heap allocation, no per-node
 * memory management.  Each price level in the order book uses one
 * IntrusiveList to maintain FIFO order within that level.
 *
 * Intrusive lists trade type safety for performance: the caller is
 * responsible for ensuring that an Order outlives its presence in
 * the list and is not simultaneously in multiple lists.
 */

#pragma once

#include "types.hpp"

namespace matching {

class IntrusiveList {
private:
    Order* head_ = nullptr;  ///< First order (MRU for push_back, FIFO front for matching).
    Order* tail_ = nullptr;  ///< Last order (most recently inserted).
    std::size_t size_ = 0;   ///< Number of orders currently in this level.

public:
    /** @brief Append an order to the tail of the list (FIFO). */
    void push_back(Order& o) {
        o.prev = tail_;
        o.next = nullptr;

        if (tail_)      tail_->next = &o;
        else            head_ = &o;
        tail_ = &o;

        ++size_;
    }

    /** @brief Remove a specific order from the list by splicing its neighbours. */
    void erase(Order& o) {
        if (o.prev) o.prev->next = o.next;
        else        head_ = o.next;
        if (o.next) o.next->prev = o.prev;
        else        tail_ = o.prev;

        o.prev = o.next = nullptr;
        --size_;
    }

    /** @brief True when no orders are in this level. */
    [[nodiscard]] bool empty() const { return head_ == nullptr; }

    /** @brief Reference to the oldest order (front of FIFO). */
    [[nodiscard]] Order& front() const { return *head_; }

    /** @brief Number of orders at this price level. */
    [[nodiscard]] std::size_t size() const { return size_; }

    const Order* begin() const { return head_; }
    Order* begin() { return head_; }
};

}  // namespace matching
