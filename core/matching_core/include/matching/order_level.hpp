#pragma once

#include "matching/types.hpp"
#include "matching/order_chunk_pool.hpp"


namespace matching {


class OrderLevel {
private:
    OrderChunkPool* chunk_pool_ = nullptr;
    OrderChunkPool::Chunk* chunk_head_ = nullptr;
    
    Order* free_order_head_ = nullptr;
    Order* head_ = nullptr;
    Order* tail_ = nullptr;
    std::size_t size_ = 0;

    // attach a new chunk to current chunk list
    void attach_chunk(OrderChunkPool::Chunk* chunk) noexcept;
    // realse the chunks hold by current OrderLevel
    void release_chunks() noexcept;

public:
    // --- Constructor ---
    explicit OrderLevel(OrderChunkPool* chunk_pool);
    // must explicityly specify the capacty of order level
    OrderLevel() = delete;
    // Note we must explicitly release order chunk pool
    ~OrderLevel();

    // Move constructors are deleted, since once moved,
    // the parentlevel pointer in Order will be invalidated
    OrderLevel(OrderLevel&& other) = delete;
    OrderLevel& operator=(OrderLevel&& other) = delete;

    // Prevent copy constructor (two pointers pointing to the same order pool)
    OrderLevel(const OrderLevel&) = delete;
    OrderLevel& operator=(const OrderLevel&) = delete;



    [[nodiscard]] Order* allocate() noexcept;   // Get an empty slot from the top of stack, if return nullptr, the pool is full

    void push_back(Order& o) noexcept;

    void remove(Order& o) noexcept;

    [[nodiscard]] bool empty() const { return head_ == nullptr; }

    [[nodiscard]] Order& front() const { return *head_; }
    
    [[nodiscard]] std::size_t size() const { return size_; }

    const Order* begin() const { return head_; };
    Order* begin() { return head_; };
};

}   // namespace matching