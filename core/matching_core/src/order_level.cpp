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
    if (available_head_ == nullptr) {   // do not have any empty chunks
        OrderChunkPool::Chunk* chunk = chunk_pool_->acquire();
        if (chunk == nullptr)   // chunk pool is full
            return nullptr;
        
        attach_chunk(chunk);
    }


    OrderChunkPool::Slot* next_free_slot = available_head_->get_free_slot();
    Order* o = &next_free_slot->order;
    
    // this chunk is full
    if (available_head_->free_count == 0)
        unlink_available(OrderChunkPool::chunk_from_order(o));

    // this chunk is full
    if (chunk->free_count == 0) {
        available_head_ = chunk->next_available;
        chunk->next_available = nullptr;
    }

    order->prev = nullptr;
    order->next = nullptr;
    order->parent_level = this;

    return order;
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

    // Calculate the chunk idx/ slot idx from address offset
    Chunk* chunk = chunk_pool_->chunk_from_order(&o);
    const std::uint16_t slot = chunk_pool_->slot_from_order(chunk, &o);

    assert(chunk->free_count < OrderChunkPool::kChunkSize);

    if (chunk->free_count == 0) {
        chunk->next_available = available_head_;
        available_head_ = chunk;
    }

    // remove o from memory pool
    o.prev = nullptr;
    o.next = nullptr;
    o.parent_level = nullptr;
}

void OrderLevel::attach_chunk(OrderChunkPool::Chunk* chunk) noexcept {
    assert(chunk != nullptr);

    chunk->next_owned = chunk_head_;
    chunk_head_ = chunk;

    chunk->next_available = available_head_;
    available_head_ = chunk;
}

void OrderLevel::release_chunks() noexcept {
    if (chunk_pool_ != nullptr && owned_head_ != nullptr) {
        chunk_pool_->release_chain(owned_head_);
    }

    owned_head_ = nullptr;
    available_head_ = nullptr;
    head_ = nullptr;
    tail_ = nullptr;
    size_ = 0;
}

}   // namespace matching