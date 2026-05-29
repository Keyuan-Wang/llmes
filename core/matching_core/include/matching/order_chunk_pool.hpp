#pragma once

#include <array>
#include <cstdint>
#include <memory>


#include "matching/types.hpp"


namespace matching {

using ChunkIndex = std::uint32_t;
using SlotIndex = std::uint16_t;

inline constexpr ChunkIndex kInvalidChunk = UINT32_MAX;

class OrderChunkPool {
public:
    static constexpr std::size_t kChunkSize = 256;

    struct Chunk {
        std::array<Order, kChunkSize> orders;
    };

    struct ChunkMeta {
        std::array<SlotIndex, kChunkSize> free_stack{};
        SlotIndex free_count = kChunkSize;

        ChunkIndex next_owned = kInvalidChunk;
        ChunkIndex next_available = kInvalidChunk;
    };

    explicit OrderChunkPool(std::size_t order_capacity);

    // get new free chunk 
    Chunk* acquire() noexcept;
    // release chunk list hold by a price level
    void release_chain(Chunk* head) noexcept;

    // delete all copy and move constructors to prevent memory leak
    OrderChunkPool(const OrderChunkPool&) = delete;
    OrderChunkPool& operator=(const OrderChunkPool&) = delete;

    OrderChunkPool(OrderChunkPool&&) = delete;
    OrderChunkPool& operator=(OrderChunkPool&&) = delete;

private:
    std::size_t chunk_count_ = 0;
    std::unique_ptr<Chunk[]> chunks_;   // chunks_ should have fixed size
    Chunk* free_chunk_head_ = nullptr;
};

}   // namespace matching