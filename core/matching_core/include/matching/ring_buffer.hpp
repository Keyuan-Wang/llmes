#pragma once

#include "price_level.hpp"
#include "types.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <bit>


namespace matching {

template <bool IsAsk>
class RingBuffer {
    static constexpr std::size_t RingSize   = 16;
    static constexpr std::size_t kMask      = RingSize - 1;
    static constexpr std::uint64_t kValid   = (1ull << RingSize) - 1;
    static constexpr std::int64_t kNoPrice  = INT64_MIN;

    struct Slot { std::int64_t price;   PriceLevel* level; };    // price and level share the same cache line

    std::int64_t    best_price_         = kNoPrice;         // cached best price anchor
    std::size_t     best_price_anchor_  = 0;                // best price idx
    std::uint64_t   live_mask_          = 0;                // bit i <=> slots_[i]
    std::array<Slot, RingSize> slots_;
    
public:
    // distance between best price and price, the smaller the better, rank < 0 means new best price
    [[gnu::always_inline]] std::int64_t rank(std::int64_t price) const noexcept {
        if constexpr (IsAsk)    return price - best_price_;
        else                    return best_price_ - price;
    }
    // check if rank in hot ring window, for r < 0, static_cast<std::uint64_t>(r) returns very large value so returns false
    bool in_hot_window(std::int64_t r) const noexcept { return static_cast<std::uint64_t>(r) < RingSize; }
    // return the circular idx of rank r
    std::size_t idx_of(std::int64_t r) const noexcept { return (best_price_anchor_ + static_cast<std::size_t>(r)) & kMask; }

    // hot path
    bool empty()                const noexcept { return live_mask_ == 0; }
    std::int64_t best_price()   const noexcept { return best_price_; }
    PriceLevel*  best_level()   const noexcept { return slots_[best_price_anchor_].level; }
    Slot&        slot(std::size_t i)  noexcept { return slots_[i]; }


    void insert(std::size_t idx, std::int64_t price, PriceLevel* level) noexcept {
        slots_[idx] = {price, level};   
        live_mask_ |= (1ull << idx);
    }

    PriceLevel* evict(std::size_t idx) noexcept {       // clear slot_[idx], return outdated PriceLevel* to cold map
        live_mask_ &= ~(1ull << idx);
        slots_[idx].price = kNoPrice;
        return std::exchange(slots_[idx].level, nullptr);
    }
    // start from best_price_anchor_, find the offset of next live bit for the "warse" direction, -1 means the ring buffer is empty
    int next_live_offset() const noexcept {
        std::uint64_t m = std::rotr(live_mask_ & kValid, best_price_anchor_) & kValid;
        return m ? std::countr_zero(m) : -1;
    }

    void set_anchor(std::size_t idx, std::int64_t price) noexcept { best_price_anchor_ = idx; best_price_ = price; }
    void reset() noexcept { live_mask_ = 0; best_price_ = kNoPrice; }
}

}   // namespace matching