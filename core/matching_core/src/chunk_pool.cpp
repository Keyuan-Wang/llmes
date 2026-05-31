#include "matching/chunk_pool.hpp"

#include <cassert>
#include <cstddef>


namespace matching {


inline std::size_t OrderChunkPool::chunk_count_for (std::size_t order_capacity) noexcept {
    return (order_capacity + OrderChunkPool::kChunkSize - 1) / OrderChunkPool::kChunkSize;  // round up
}


void OrderChunkPool::init_free_list(Chunk& chunk) noexcept {
    chunk.free_head = &chunk.slots[0];

    for (std::size_t i = 0; i < kChunkSize - 1; ++i) {
        chunk.slots[i].next_free = &chunk.slots[i+1];
    }

    chunk.slots[kChunkSize - 1].next_free = nullptr;
    chunk.free_count = kChunkSize;
}

OrderChunkPool::OrderChunkPool(std::size_t order_capacity)
    : chunk_count_(chunk_count_for(order_capacity))
    , chunks_(std::make_unique<Chunk[]>(chunk_count_)) {

    for (std::size_t i = 0; i < chunk_count_; ++i) {
        Chunk& chunk = chunks_[i];
        
        init_free_list(chunk);
    
        chunk.next_free_pool = free_chunk_head_;
        free_chunk_head_ = &chunk;
    }
}


OrderChunkPool::Chunk* OrderChunkPool::acquire() noexcept {
    Chunk* chunk = free_chunk_head_;
    if (chunk == nullptr)   // OrderChunkPool is full
        return nullptr;

    free_chunk_head_ = chunk->next_free_pool;

    chunk->next_free_pool = nullptr;

    assert(chunk->free_count == kChunkSize);    // this chunk must be an empty chunk
    return chunk;
}


void OrderChunkPool::release(Chunk* chunk) noexcept {
    assert(chunk->free_count == kChunkSize);    // this chunk must be an empty chunk

    chunk->prev_available = nullptr;
    chunk->next_available = nullptr;
    chunk->next_free_pool = free_chunk_head_;
    
    free_chunk_head_ = chunk;
}


OrderChunkPool::Chunk* OrderChunkPool::chunk_from_order(const Order* order) noexcept {
    const auto* base = reinterpret_cast<const std::byte*>(chunks_.get());
    const auto* p = reinterpret_cast<const std::byte*>(order);

    const auto offset = static_cast<std::size_t>(p - base);
    const auto chunk_index = offset / sizeof(Chunk);

    assert(chunk_index < chunk_count_);

    Chunk* chunk = chunks_.get() + chunk_index;

    return chunk;
}


OrderChunkPool::Slot* OrderChunkPool::slot_from_order(Chunk* chunk, const Order* order) const noexcept {
    const auto* chunk_base = reinterpret_cast<const std::byte*>(chunk);
    const auto* order_addr = reinterpret_cast<const std::byte*>(order);

    const auto offset = static_cast<std::size_t>(order_addr - chunk_base);

    const auto slot_index = offset / sizeof(Slot);
    assert(slot_index < kChunkSize);

    Slot* slot = &chunk->slots[slot_index];
    assert(&slot->order == order);

    return &chunk->slots[slot_index];
}

}   // namespace matching