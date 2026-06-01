/**
 * @file order_book.cpp
 * @brief Implementation of @ref matching::OrderBook (Phase 1: map + list, FIFO per level).
 */

#include <algorithm>
#include <cassert>
#include <chrono>

#include "matching/add_rest_stage_profile.hpp"
#include "matching/order_book.hpp"
#include "matching/intrusive_list.hpp"
#include "matching/order_pool.hpp"
#include "matching/types.hpp"

#ifndef LLMES_PROFILE_ADD_REST_STAGES
#define LLMES_PROFILE_ADD_REST_STAGES 0
#endif

#if LLMES_PROFILE_ADD_REST_STAGES && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#endif

namespace matching {

namespace {

#if LLMES_PROFILE_ADD_REST_STAGES
using ProfileClock = std::chrono::steady_clock;

std::uint64_t ReadProfileCycles() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    return 0;
#endif
}

class AddRestCallProfile {
public:
    explicit AddRestCallProfile(bool enabled) noexcept : enabled_(enabled) {}

    [[nodiscard]] bool enabled() const noexcept {
        return enabled_;
    }

    void Add(AddRestStage stage, std::uint64_t ns, std::uint64_t cycles) noexcept {
        const std::size_t idx = static_cast<std::size_t>(stage);
        stage_ns_[idx] += ns;
        stage_cycles_[idx] += cycles;
    }

    void Commit() const noexcept {
        if (enabled_) {
            RecordAddRestStageProfile(stage_ns_, stage_cycles_);
        }
    }

private:
    bool enabled_ = false;
    std::array<std::uint64_t, kAddRestStageCount> stage_ns_{};
    std::array<std::uint64_t, kAddRestStageCount> stage_cycles_{};
};

class ScopedAddRestStage {
public:
    ScopedAddRestStage(AddRestCallProfile& profile, AddRestStage stage) noexcept
        : profile_(profile), stage_(stage), enabled_(profile.enabled()) {
        if (enabled_) {
            start_cycles_ = ReadProfileCycles();
            start_time_ = ProfileClock::now();
        }
    }

    ~ScopedAddRestStage() {
        if (!enabled_) return;
        const auto end_time = ProfileClock::now();
        const std::uint64_t end_cycles = ReadProfileCycles();
        const std::uint64_t ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end_time - start_time_).count());
        const std::uint64_t cycles =
            (end_cycles >= start_cycles_) ? (end_cycles - start_cycles_) : 0;
        profile_.Add(stage_, ns, cycles);
    }

private:
    AddRestCallProfile& profile_;
    AddRestStage stage_;
    bool enabled_ = false;
    ProfileClock::time_point start_time_{};
    std::uint64_t start_cycles_ = 0;
};
#endif

/**
 * @brief Whether a limit order crosses the opposite side's best quote.
 *
 * @param taker_side            Side of the incoming limit order.
 * @param limit_price           Limit price of the taker.
 * @param best_opposite_price   Best price on the opposite book (lowest ask or highest bid).
 * @return True if at least one share can match at @p best_opposite_price.
 */
bool can_cross_limit(Side taker_side, std::int64_t limit_price, std::int64_t best_opposite_price) {
    if (taker_side == Side::Buy) {
        return limit_price >= best_opposite_price;
    }
    return limit_price <= best_opposite_price;
}

}  // namespace

/**
 * @copydoc OrderBook::cancel_order
 */
ErrorCode OrderBook::cancel_order(std::uint64_t order_id) {
    auto it = id_to_order_.find(order_id);

    if (it != id_to_order_.end()) {
        Order* o = it->second;
        o->parent_level->erase(*o);
        pool_.release(o);
        id_to_order_.erase(order_id);
        return ErrorCode::Success;
    }

    pending_cancel_ids_.insert(order_id);
    return ErrorCode::UnknownOrderId;
}

/**
 * @copydoc OrderBook::modify_order
 */
AddResult OrderBook::modify_order(std::uint64_t order_id, Side side, std::int64_t price,
                                  std::uint64_t quantity, std::uint64_t timestamp) {
    // Remove existing order with this id if present (no side effects on pending_cancel_ids_).
    auto it = id_to_order_.find(order_id);
    if (it != id_to_order_.end()) {
        Order* o = it->second;
        o->parent_level->erase(*o);
        pool_.release(o);
        id_to_order_.erase(order_id);
    }

    // A prior cancel that landed in pending_cancel_ids_ is overridden by modify.
    pending_cancel_ids_.erase(order_id);

    // Delegate to add_limit_order — both duplicate and pending-cancel checks
    // will pass since we just cleaned up state for this id.
    return add_limit_order(order_id, side, price, quantity, timestamp);
}

/**
 * @copydoc OrderBook::add_limit_order
 */
AddResult OrderBook::add_limit_order(std::uint64_t order_id, Side side, std::int64_t price,
                                     std::uint64_t quantity, std::uint64_t timestamp) {
#if LLMES_PROFILE_ADD_REST_STAGES
    AddRestCallProfile add_rest_profile(AddRestStageProfileEnabled());
#endif
    AddResult out{};

    {
#if LLMES_PROFILE_ADD_REST_STAGES
        ScopedAddRestStage stage(add_rest_profile, AddRestStage::kValidation);
#endif
        out.initial_quantity = quantity;

        if (quantity == 0) {
            out.code = ErrorCode::InvalidQuantity;
            return out;
        }

        if (pending_cancel_ids_.contains(order_id)) {
            out.code = ErrorCode::PendingCancelExists;
            out.remaining_quantity = quantity;
            return out;
        }

        if (id_to_order_.contains(order_id)) {
            out.code = ErrorCode::DuplicateOrderId;
            out.remaining_quantity = quantity;
            return out;
        }
    }

    std::uint64_t remaining = quantity;

    // Consume opposite-side liquidity while the limit price permits crossing.
    auto match_against = [&](auto& opposite_book) {
        while (remaining > 0 && !opposite_book.empty()) {
            const std::int64_t best_price = opposite_book.best_price();
            if (!can_cross_limit(side, price, best_price)) {
                break;
            }

            auto& price_level = opposite_book.best_level();

            while (remaining > 0 && !price_level.empty()) {
                Order& maker = price_level.front();

                const std::uint64_t fill = std::min(remaining, maker.quantity);

                out.trades.emplace_back(order_id, maker.id, maker.price, fill);

                maker.quantity -= fill;
                remaining -= fill;
                out.filled_quantity += fill;

                if (maker.quantity == 0) {
                    id_to_order_.erase(maker.id);

                    Order* maker_ptr = &maker;
                    price_level.erase(*maker_ptr);
                    pool_.release(maker_ptr);
                }
            }
            
            if (price_level.empty())
                opposite_book.erase_best();
        }
    };

    {
#if LLMES_PROFILE_ADD_REST_STAGES
        ScopedAddRestStage stage(add_rest_profile, AddRestStage::kMatch);
#endif
        if (side == Side::Buy) {
            match_against(asks_);
        } else {
            match_against(bids_);
        }
    }

    out.remaining_quantity = remaining;

    if (remaining == 0) {
        out.code = ErrorCode::Success;
        return out;
    }

    // add remaining limit order to book
    Order* node = nullptr;
    {
#if LLMES_PROFILE_ADD_REST_STAGES
        ScopedAddRestStage stage(add_rest_profile, AddRestStage::kPoolAcquire);
#endif
        node = pool_.acquire();
    }
    // TODO: WHAT IF POOL IS ALREADY EMPTY?
    assert(node != nullptr);

    {
#if LLMES_PROFILE_ADD_REST_STAGES
        ScopedAddRestStage stage(add_rest_profile, AddRestStage::kNodeInit);
#endif
        *node = {order_id, price, remaining, timestamp};
    }

    PriceLevel* level = nullptr;
    {
#if LLMES_PROFILE_ADD_REST_STAGES
        const bool level_lookup_profile_enabled = add_rest_profile.enabled();
        const auto level_lookup_start_time =
            level_lookup_profile_enabled ? ProfileClock::now() : ProfileClock::time_point{};
        const std::uint64_t level_lookup_start_cycles =
            level_lookup_profile_enabled ? ReadProfileCycles() : 0;
#endif
        [[maybe_unused]] bool inserted = false;
        if (side == Side::Buy) {
            auto [lookup_level, was_inserted] = bids_.get_or_create(price);
            level = lookup_level;
            inserted = was_inserted;
        } else {
            auto [lookup_level, was_inserted] = asks_.get_or_create(price);
            level = lookup_level;
            inserted = was_inserted;
        }
#if LLMES_PROFILE_ADD_REST_STAGES
        if (level_lookup_profile_enabled) {
            const auto level_lookup_end_time = ProfileClock::now();
            const std::uint64_t level_lookup_end_cycles = ReadProfileCycles();
            const std::uint64_t level_lookup_ns =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        level_lookup_end_time - level_lookup_start_time).count());
            const std::uint64_t level_lookup_cycles =
                (level_lookup_end_cycles >= level_lookup_start_cycles)
                    ? (level_lookup_end_cycles - level_lookup_start_cycles)
                    : 0;
            add_rest_profile.Add(
                inserted ? AddRestStage::kLevelCreateNew : AddRestStage::kLevelLookupExisting,
                level_lookup_ns,
                level_lookup_cycles);
        }
#endif
    }

    {
#if LLMES_PROFILE_ADD_REST_STAGES
        ScopedAddRestStage stage(add_rest_profile, AddRestStage::kFifoAppend);
#endif
        level->push_back(*node);
        node->parent_level = level;
    }

    {
#if LLMES_PROFILE_ADD_REST_STAGES
        ScopedAddRestStage stage(add_rest_profile, AddRestStage::kIdIndexInsert);
#endif
        id_to_order_.emplace(order_id, node);
    }

#if LLMES_PROFILE_ADD_REST_STAGES
    add_rest_profile.Commit();
#endif

    // output
    out.code = ErrorCode::Success;
    return out;
}

/**
 * @copydoc OrderBook::add_market_order
 */
AddResult OrderBook::add_market_order(std::uint64_t order_id, Side side, std::uint64_t quantity,
                                      std::uint64_t timestamp) {
    (void)timestamp;

    AddResult out{};
    out.initial_quantity = quantity;

    if (quantity == 0) {
        out.code = ErrorCode::InvalidQuantity;
        return out;
    }

    if (pending_cancel_ids_.contains(order_id)) {
        out.code = ErrorCode::PendingCancelExists;
        out.remaining_quantity = quantity;
        return out;
    }

    if (id_to_order_.contains(order_id)) {
        out.code = ErrorCode::DuplicateOrderId;
        out.remaining_quantity = quantity;
        return out;
    }

    std::uint64_t remaining = quantity;

    auto match_against = [&](auto& opposite_book) {
        while (remaining > 0 && !opposite_book.empty()) {
            auto& price_level = opposite_book.best_level();

            while (remaining > 0 && !price_level.empty()) {
                Order& maker = price_level.front();

                const std::uint64_t fill = std::min(remaining, maker.quantity);

                out.trades.emplace_back(order_id, maker.id, maker.price, fill);

                maker.quantity -= fill;
                remaining -= fill;
                out.filled_quantity += fill;

                if (maker.quantity == 0) {
                    id_to_order_.erase(maker.id);

                    Order* maker_ptr = &maker;
                    price_level.erase(*maker_ptr);
                    pool_.release(maker_ptr);
                }
            }

            if (price_level.empty())
                opposite_book.erase_best();
        }
    };

    if (side == Side::Buy) {
        match_against(asks_);
    } else {
        match_against(bids_);
    }

    out.remaining_quantity = remaining;

    if (remaining == 0) {
        out.code = ErrorCode::Success;
        return out;
    }

    out.code = ErrorCode::MarketRemainderCancelled;
    return out;
}

}  // namespace matching
