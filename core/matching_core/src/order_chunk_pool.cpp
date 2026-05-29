#include "matching/order_chunk_pool.hpp"
#include "matching/types.hpp"

#include <cassert>


namespace matching {


namespace {

inline std::size_t chunk_count_for (std::size_t order_capacity) noexcept {
    return (order_capacity + OrderChunkPool::kChunkSize - 1) / OrderChunkPool::kChunkSize;  // round up
}

}   // namespace

OrderChunkPool::OrderChunkPool(std::size_t order_capacity)
    : chunks_(chunk_count_for(order_capacity)) {
    for (auto& chunk : chunks_) {
        chunk.next = free_chunk_head_;
        free_chunk_head_ = &chunk;

        chunk.orders[0].next = &chunk.orders[1];
        for (std::size_t i = 1; i < kChunkSize - 1; ++i) {
            Order& o = chunk.orders[i];

            o.prev = &chunk.orders[i-1];
            o.next = &chunk.orders[i+1];
        }
        chunk.orders[kChunkSize - 1].prev = &chunk.orders[kChunkSize - 2];
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