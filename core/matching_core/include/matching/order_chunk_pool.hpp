#pragma once

#include <array>
#include <memory>


#include "matching/types.hpp"


namespace matching {

class OrderChunkPool {
public:
    // delete all copy and move constructors to prevent memory leak
    OrderChunkPool(const OrderChunkPool&) = delete;
    OrderChunkPool& operator=(const OrderChunkPool&) = delete;

    OrderChunkPool(OrderChunkPool&&) = delete;
    OrderChunkPool& operator=(OrderChunkPool&&) = delete;


    static constexpr std::size_t kChunkSize = 256;

    struct Chunk {
        std::array<Order, kChunkSize> orders;
        Chunk* next = nullptr;
    };

    explicit OrderChunkPool(std::size_t order_capacity);

    // get new free chunk 
    Chunk* acquire() noexcept;
    // release chunk list hold by a price level
    void release_chain(Chunk* head) noexcept;

private:
    std::size_t chunk_count_ = 0;
    std::unique_ptr<Chunk[]> chunks_;   // chunks_ should have fixed size
    Chunk* free_chunk_head_ = nullptr;
};

}   // namespace matching