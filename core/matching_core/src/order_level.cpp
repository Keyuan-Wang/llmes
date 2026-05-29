#include "matching/order_level.hpp"
#include "matching/order_chunk_pool.hpp"

#include <cassert>


namespace matching {


OrderLevel::OrderLevel(OrderChunkPool* chunk_pool)
    : chunk_pool_(chunk_pool) {
    assert(chunk_pool_ != nullptr);
}


OrderLevel::~OrderLevel() {
    release_chunks();
}


[[nodiscard]] Order* OrderLevel::allocate() noexcept {
    if (free_order_head_ == nullptr) {
        OrderChunkPool::Chunk* chunk = chunk_pool_->acquire();
        if (chunk == nullptr)   // chunk pool is full
            return nullptr;
        
        attach_chunk(chunk);
    }

    Order* node = free_order_head_;
    free_order_head_ = node->next;

    node->prev = nullptr;
    node->next = nullptr;
    node->parent_level = this;

    return node;
}


void OrderLevel::push_back(Order& o) noexcept {
    assert(o->parent_level == this);

    o.prev = tail_;
    o.next = nullptr;

    if (tail_)      tail_->next = &o;
    else            head_ = &o;
    tail_ = &o;

    ++size_;
}

void OrderLevel::remove(Order& o) noexcept {
    assert(o->parent_level == this);
    assert(size_ > 0);
    // remove o from intrusive list
    if (o.prev) o.prev->next = o.next;
    else        head_ = o.next;
    if (o.next) o.next->prev = o.prev;
    else        tail_ = o.prev;
    
    o.prev = nullptr;
    --size_;

    // remove o from memory pool
    o.next = free_order_head_;
    o.parent_level = this;
    free_order_head_ = &o;
}

void OrderLevel::attach_chunk(OrderChunkPool::Chunk* chunk) noexcept {
    assert(chunk != nullptr);

    chunk->next = chunk_head_;
    chunk_head_ = chunk;

    free_order_head_ = &chunk->orders[0];
}

void OrderLevel::release_chunks() noexcept {
    chunk_pool_->release_chain(chunk_head_);

    head_ = nullptr;
    tail_ = nullptr;
    size_ = 0;
}

}   // namespace matching