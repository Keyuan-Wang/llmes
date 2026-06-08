#include "matching/occupancy_tree.hpp"

#include <bit>
#include <optional>


namespace matching {

OccupancyTree::OccupancyTree(std::size_t bit_count) : bit_count_(bit_count) {
    assert(bit_count_ > 0 && (bit_count_ % 64) == 0);

    std::size_t bits = bit_count_;
    for(;;) {
        bit_counts_.push_back(bits);    // to identify padding bit
        levels_.push_back(std::vector<std::uint64_t>(word_count(bits), 0));

        if (word_count(bits) == 1)  break;

        bits = word_count(bits);
    }
}

void OccupancyTree::set(std::size_t bit) noexcept {
    assert(bit < bit_count_);

    std::size_t idx = bit;

    // which level?
    for (std::size_t level = 0; level < levels_.size(); ++ level) {
        // which word?
        const std::size_t word_idx = idx / kWordBits;       // leave for compiler optimization
        // which bit?
        const std::size_t bit_idx = idx & (kWordBits - 1);  // leave for compiler optimization
        const std::uint64_t mask = static_cast<std::uint64_t>(1) << bit_idx;

        std::uint64_t& word = levels_[level][word_idx];

        const bool was_empty = word == 0;

        word |= mask;       // set word[bit_idx] to 1

        if (!was_empty) [[likely]]  break;  // the upper level word is already 1

        idx = word_idx;     // the word idx of current level is the bit index of next level
    }
}


void OccupancyTree::clear(std::size_t bit) noexcept {
    assert(bit < bit_count_);

    std::size_t idx = bit;

    // which level?
    for (std::size_t level = 0; level < levels_.size(); ++ level) {
        // which word?
        const std::size_t word_idx = idx / kWordBits;       // leave for compiler optimization
        // which bit?
        const std::size_t bit_idx = idx & (kWordBits - 1);  // leave for compiler optimization
        const std::uint64_t mask = static_cast<std::uint64_t>(1) << bit_idx;

        std::uint64_t& word = levels_[level][word_idx];

        assert((word & mask) != 0 && "clear() requires a currently set bit");

        word &= ~mask;      // set word[bit_idx] to 0

        if (word != 0) [[likely]]  break;  // other bits are still valid, stop propogating

        idx = word_idx;     // the word idx of current level is the bit index of next level
    }
}



std::size_t OccupancyTree::find_next_set(std::size_t level, std::size_t start_bit) const noexcept {
    assert(level < levels_.size());
    assert(start_bit < bit_counts_[level]);

    const std::size_t word_idx = start_bit / kWordBits;
    const std::size_t bit_idx = start_bit & (kWordBits - 1);

    std::uint64_t word = levels_[level][word_idx] & mask_high(bit_idx);     // mask from 63th bit to bit_idxth bit
    // found the next best price in current word
    // for benchmark case, this is likely to happen as best price shifts slowly
    if (word != 0) [[likely]] {             
        // find the index of next best price
        const std::size_t found = word_idx * kWordBits + static_cast<std::size_t>(std::countr_zero(word));
        // is it a valid index?
        assert(found < bit_counts_[level]);
        return found;
    }

    // whole book empty, this will never happen in the benchmark case
    assert(level + 1 < levels_.size());
    assert(word_idx + 1 < bit_counts_[level + 1]);

    // if not found, go to next level and find the word idx
    std::size_t next_word_idx = find_next_set(level + 1, word_idx + 1);

    word = levels_[level][next_word_idx];
    assert(word != 0);

    const std::size_t found = next_word_idx * kWordBits + static_cast<std::size_t>(std::countr_zero(word));

    assert(found < bit_counts_[level]);

    return found;
}




std::size_t OccupancyTree::find_prev_set(std::size_t level, std::size_t start_bit) const noexcept {
    assert(level < levels_.size());
    assert(start_bit < bit_counts_[level]);

    const std::size_t word_idx = start_bit / kWordBits;
    const std::size_t bit_idx = start_bit & (kWordBits - 1);

    std::uint64_t word = levels_[level][word_idx] & mask_low(bit_idx);     // mask from 0th bit to bit_idxth bit
    // found the next best price in current word
    // for benchmark case, this is likely to happen as best price shifts slowly
    if (word != 0) [[likely]] {             
        // find the index of next best price
        const std::size_t found = word_idx * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(word)));
        // is it a valid index?
        assert(found < bit_counts_[level]);
        return found;
    }

    // whole book empty, this will never happen in the benchmark case
    assert(level + 1 < levels_.size());
    assert(word_idx > 0);

    // if not found, go to next level and find the word idx
    std::size_t prev_word_idx = find_prev_set(level + 1, word_idx - 1);

    word = levels_[level][prev_word_idx];
    assert(word != 0);


    const std::size_t found = prev_word_idx * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(word)));

    assert(found < bit_counts_[level]);

    return found;
}


}   // namespace matching
