#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>


#include "matching/types.hpp"

namespace matching {

class Chunk {
public:
    Order* allocate_slot() noexcept;
    void release_slot(Order* order) noexcept;

    bool full() const noexcept;
    bool empty() const noexcept;

    void link_available(Chunk* chunk) noexcept;
    void unlink_available(Chunk* chunk) noexcept;

    void insert_empty_chunk(Chunk* chunk) noexcept;

private:
    struct Slot {
        Order order;
        Slot* next_free = nullptr;
    };

    static constexpr std::size_t kChunkSize = 256;
    std::array<Slot, kChunkSize> slots_;

    Slot* free_head_ = nullptr;
    std::uint16_t free_count_ = kChunkSize;

    Chunk* prev_available_ = nullptr;
    Chunk* next_available_ = nullptr;
    Chunk* next_free_pool_ = nullptr;
};


class ChunkPool {
public :
    explicit ChunkPool(std::size_t order_capacity);

    Chunk* acquire_empty_chunk() noexcept;
    void release_empty_chunk(Chunk* chunk) noexcept;

    Chunk* chunk_from_order(Order* order) noexcept;

    // delete all copy and move constructors to prevent memory leak
    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;

    ChunkPool(ChunkPool&&) = delete;
    ChunkPool& operator=(ChunkPool&&) = delete;

private:
    std::size_t chunk_count_ = 0;
    std::unique_ptr<Chunk[]> chunks_;
    Chunk* free_chunk_head_ = nullptr;
};

}   // namespace matching