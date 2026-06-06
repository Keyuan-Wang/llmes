#include "matching/pricelevel_pool.hpp"

#include <cassert>

namespace matching {

PriceLevelPool::PriceLevelPool(std::size_t capacity) {
    pool_.resize(capacity);

    Slot* p = nullptr;
    for (auto& slot : pool_) {
        slot.next = p;
        free_head_ = &slot;

        p = &slot;
    }
}

void PriceLevelPool::release(PriceLevel* level) {
    assert(level != nullptr && level->empty());

    level->reset();

    // level points at Slot::level (first member); recover the Slot header.
    auto* slot = reinterpret_cast<Slot*>(level);

    slot->next = free_head_;
    free_head_ = slot;
}

}   // namespace matching