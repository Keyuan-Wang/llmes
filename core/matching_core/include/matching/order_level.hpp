#pragma once

#include "matching/types.hpp"


namespace matching {

class OrderLevel {
private:
    std::vector<Order> pool_;
    Order* free_head_ = nullptr;
    Order* head_ = nullptr;
    Order* tail_ = nullptr;
    std::size_t size_ = 0;

public:
    // --- Constructor ---
    explicit OrderLevel(std::size_t capacity);
    // must explicityly specify the capacty of order level
    OrderLevel() = delete;

    // Move constructor
    OrderLevel(OrderLevel&& other) noexcept;
    // Move operator
    OrderLevel& operator=(OrderLevel&& other) noexcept;
    // Prevent copy constructor (two pointers pointing to the same order pool)
    OrderLevel(const OrderLevel&) = delete;
    OrderLevel& operator=(const OrderLevel&) = delete;



    [[nodiscard]] Order* allocate();          // Get an empty slot from the top of stack, if return nullptr, the pool is full

    void push_back(Order& o);

    void remove(Order& o);
    
    void clear();

    [[nodiscard]] std::size_t capacity() const noexcept { return pool_.size(); };

    [[nodiscard]] bool empty() const { return head_ == nullptr; }

    [[nodiscard]] Order& front() const { return *head_; }
    
    [[nodiscard]] std::size_t size() const { return size_; }

    const Order* begin() const { return head_; };
    Order* begin() { return head_; };
};

}