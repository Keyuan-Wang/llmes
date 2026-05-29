#include "matching/order_chunk_pool.hpp"
#include "matching/types.hpp"

#include <cassert>
#include <memory>


namespace matching {


namespace {

inline std::size_t chunk_count_for (std::size_t order_capacity) noexcept {
    return (order_capacity + OrderChunkPool::kChunkSize - 1) / OrderChunkPool::kChunkSize;  // round up
}

}   // namespace

OrderChunkPool::OrderChunkPool(std::size_t order_capacity)
    : chunk_count_(chunk_count_for(order_capacity))
    , chunks_(std::make_unique<Chunk[]>(chunk_count_)) {

    for (std::size_t i = 0; i < chunk_count_; ++i) {
        Chunk& chunk = chunks_[i];
        
        for (std::uint16_t j = 0; j < kChunkSize; ++j)
            chunk.free_stack[j] = j;
    
        chunk.next_pool = free_chunk_head_;
        free_chunk_head_ = &chunk;
    }
}


OrderChunkPool::Chunk* OrderChunkPool::acquire() noexcept {
    Chunk* chunk = free_chunk_head_;
    if (chunk == nullptr)   // OrderChunkPool is full
        return nullptr;
    
    free_chunk_head_ = chunk->next;
    chunk->next = nullptr;
    return chunk;
}


void OrderChunkPool::release_chain(Chunk* head) noexcept {
    while (head != nullptr) {
        Chunk* next = head->next;
        head->next = free_chunk_head_;
        free_chunk_head_ = head;
        head = next;
    }
}

}   // namespace matching