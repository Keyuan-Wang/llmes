#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>


namespace matching {

class OccupancyTree {
public:
    explicit OccupancyTree(std::size_t bit_count = 65536);

    void set(std::size_t bit) noexcept;
    void clear(std::size_t bit) noexcept;

    template <bool IsAsk>
    std::size_t next_best(std::size_t start_bit) const noexcept {
        if constexpr (IsAsk)    return find_next_set(0, start_bit); // ask book, next best price is larger
        else                    return find_prev_set(0, start_bit); // bid book, next best price is smaller
    }

    // if this book is empty or not
    [[nodiscard]] bool empty() const noexcept {
        return levels_.empty() || levels_.back()[0] == 0;
    }

private:
    static constexpr std::size_t kWordBits = 64;    // size of a word

    std::size_t bit_count_ = 0;
    std::vector<std::size_t> bit_counts_;
    std::vector<std::vector<std::uint64_t>> levels_;

    [[gnu::always_inline]] std::size_t word_count(std::size_t bits) const noexcept {
        return (bits + kWordBits - 1) / kWordBits;  // leave for compiler optimization
    }

    // mask high bit_idx bits
    [[gnu::always_inline]] std::uint64_t mask_high(std::size_t bit_idx) const noexcept {
        return ~static_cast<std::uint64_t>(0) << bit_idx;
    }

    // mask low bit_idx bits
    [[gnu::always_inline]] std::uint64_t mask_low(std::size_t bit_idx) const noexcept {
        if (bit_idx == 63) [[unlikely]]  return ~static_cast<std::uint64_t>(0);
        return (static_cast<std::uint64_t>(1) << (bit_idx + 1)) - 1;
    }

    std::size_t find_next_set(std::size_t level, std::size_t start_bit) const noexcept;

    std::size_t find_prev_set(std::size_t level, std::size_t start_bit) const noexcept;

};

}   // namespace matching