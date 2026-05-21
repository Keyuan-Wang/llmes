/**
 * @file flat_hash_map.cpp
 * @brief Implementation of open-addressing flat hash map (Phase 2c).
 *
 * All documented in flat_hash_map.hpp — this file contains the out-of-line
 * method bodies for @ref matching::HashTable.
 */

#include "matching/flat_hash_map.hpp"

matching::HashTable::HashTable(std::size_t expected)
    : capacity_(next_power_of_2(expected > 4 ? expected : 4)),
      mask_(capacity_ - 1),
      slots_(capacity_) {}

bool matching::HashTable::insert(std::uint64_t key, void* value) {
    // Trigger rehash when the effective load (live + tombstone) exceeds a threshold (60%).
    // Rehash doubles capacity and purges all tombstones in one pass.
    if (size_ + tombstones_ > static_cast<std::size_t>(capacity_ * kLoadFactorMax))
        rehash();

    std::size_t idx = hash(key) & mask_;

    // Linear probe: skip occupied slots that don't match our key.
    // TOMBSTONE slots terminate the probe for insert (we can reuse them),
    // but for find/erase the probe continues past tombstones.
    while (slots_[idx].state == State::OCCUPIED) {
        if (slots_[idx].key == key) {
            slots_[idx].value = value;
            return false;       // key already existed — updated value in-place.
        }
        // Wrap around using the power-of-2 mask (avoids a modulo instruction).
        idx = (idx + 1) & mask_;
    }

    // Found an empty or tombstone slot — occupy it.
    slots_[idx] = {key, value, State::OCCUPIED};
    ++size_;
    return true;
}

void matching::HashTable::rehash() {
    // Steal the old slot array; the new one is fresh and twice as large.
    std::vector<Slot> old = std::move(slots_);
    capacity_ = next_power_of_2(capacity_ * 2);
    slots_.resize(capacity_);

    mask_ = capacity_ - 1;
    size_ = 0;
    tombstones_ = 0;   // tombstones are implicitly discarded during rehash.

    // Re-insert every live entry.  TOMBSTONE entries are skipped — they die.
    for (auto& slot : old) {
        if (slot.state == State::OCCUPIED)
            insert(slot.key, slot.value);
    }
}

void* matching::HashTable::find(std::uint64_t key) {
    std::size_t idx = hash(key) & mask_;

    // Probe until we hit an EMPTY slot (probe chain terminus).
    // TOMBSTONE slots are skipped: the key might be further down the chain.
    while (slots_[idx].state != State::EMPTY) {
        if (slots_[idx].key == key)
            return slots_[idx].value;
        idx = (idx + 1) & mask_;
    }

    return nullptr;     // key not found — hit an uninterrupted empty slot.
}

bool matching::HashTable::erase(std::uint64_t key) {
    std::size_t idx = hash(key) & mask_;

    // Same probe as find: walk past tombstones, stop at EMPTY.
    while (slots_[idx].state != State::EMPTY) {
        if (slots_[idx].key == key) {
            slots_[idx].state = State::TOMBSTONE;
            ++tombstones_;
            --size_;
            return true;
        }
        idx = (idx + 1) & mask_;
    }

    return false;       // key did not exist.
}
