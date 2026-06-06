#pragma once

#include "price_level.hpp"

#include <cassert>
#include <vector>

namespace matching {

/**
 * @brief Freelist allocator for @ref PriceLevel objects on one book side.
 *
 * @details Storage is a single @c std::vector<Slot> where each @c Slot embeds a
 * @ref PriceLevel and a freelist link. @c acquire() returns @c &(slot.level);
 * @c release() recovers the parent @c Slot from that pointer (standard-layout
 * first member) and pushes it back on the stack.
 *
 * Only @ref CachedSideBook may call @c release(). Ring slots and cold-map nodes
 * hold non-owning @c PriceLevel* into this pool.
 */
class PriceLevelPool {
private:
    struct Slot {
        PriceLevel level;
        Slot*      next = nullptr;
    };

public:
    explicit PriceLevelPool(std::size_t capacity);

    /** @return Pointer to a reset-ready level, or asserts if the pool is exhausted. */
    [[nodiscard]] [[gnu::always_inline]] inline PriceLevel* acquire() {
        assert(free_head_ != nullptr && "PriceLevelPool exhausted");

        Slot* slot = free_head_;
        free_head_ = slot->next;

        return &(slot->level);
    }

    /**
     * @brief Return an empty level to the freelist.
     * @pre @p level came from this pool and @p level->empty().
     */
    void release(PriceLevel* level);

    [[nodiscard]] std::size_t capacity() const noexcept { return pool_.size(); }

private:
    std::vector<Slot> pool_;
    Slot*             free_head_ = nullptr;
};

}   // namespace matching
