/**
 * @file order_book.cpp
 * @brief Implementation of @ref matching::OrderBook (Phase 1: map + list, FIFO per level).
 */

#include "matching/order_book.hpp"

#include <algorithm>

namespace matching {

namespace {

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
    auto try_remove = [&](auto& book) -> bool {
        for (auto level_it = book.begin(); level_it != book.end(); ++level_it) {
            auto& queue = level_it->second;
            for (auto it = queue.begin(); it != queue.end(); ++it) {
                if (it->id == order_id) {
                    queue.erase(it);
                    if (queue.empty()) {
                        book.erase(level_it);
                    }
                    return true;
                }
            }
        }
        return false;
    };

    if (try_remove(bids_) || try_remove(asks_)) {
        return ErrorCode::Success;
    }

    return ErrorCode::UnknownOrderId;
}

/**
 * @copydoc OrderBook::modify_order
 */
AddResult OrderBook::modify_order(std::uint64_t order_id, Side side, std::int64_t price,
                                  std::uint32_t quantity, std::uint64_t timestamp) {
    // Try to cancel the existing order (if found, removed from book).
    // If not found, cancel_order inserts the id into pending_cancel_ids_.
    const ErrorCode cancel_code = cancel_order(order_id);

    // A cancel-miss adds the id to pending_cancel_ids_, which would block the
    // subsequent add.  Clear it — modify overrides any prior cancel signal.
    if (cancel_code == ErrorCode::UnknownOrderId) {
    }

    // Now add as a fresh limit order — both DuplicateOrderId and
    // PendingCancelExists checks will pass since we cleaned up.
    return add_limit_order(order_id, side, price, quantity, timestamp);
}

/**
 * @copydoc OrderBook::add_limit_order
 */
AddResult OrderBook::add_limit_order(std::uint64_t order_id, Side side, std::int64_t price,
                                     std::uint32_t quantity, std::uint64_t timestamp) {
    AddResult out{};
    out.initial_quantity = quantity;

    if (quantity == 0) {
        out.code = ErrorCode::InvalidQuantity;
        return out;
    }



    std::uint64_t remaining = quantity;

    // Consume opposite-side liquidity while the limit price permits crossing.
    auto match_against = [&](auto& opposite_book) {
        while (remaining > 0 && !opposite_book.empty()) {
            const std::int64_t best_price = opposite_book.begin()->first;
            if (!can_cross_limit(side, price, best_price)) {
                break;
            }

            auto level_it = opposite_book.begin();
            auto& queue = level_it->second;

            while (remaining > 0 && !queue.empty()) {
                Order& maker = queue.front();
                const std::uint32_t fill =
                    static_cast<std::uint32_t>(std::min(remaining, maker.quantity));

                out.trades.emplace_back(order_id, maker.id, maker.price, fill);

                maker.quantity -= fill;
                remaining -= fill;
                out.filled_quantity += fill;

                if (maker.quantity == 0) {
                    queue.pop_front();
                }
            }

            if (queue.empty()) {
                opposite_book.erase(level_it);
            }
        }
    };

    if (side == Side::Buy) {
        match_against(asks_);
    } else {
        match_against(bids_);
    }

    out.remaining_quantity = static_cast<std::uint32_t>(remaining);

    if (remaining == 0) {
        out.code = ErrorCode::Success;
        return out;
    }

    const Order resting{order_id, price, remaining, timestamp};
    if (side == Side::Buy) {
        bids_[price].push_back(resting);
    } else {
        asks_[price].push_back(resting);
    }

    out.code = ErrorCode::Success;
    return out;
}

/**
 * @copydoc OrderBook::add_market_order
 */
AddResult OrderBook::add_market_order(std::uint64_t order_id, Side side, std::uint32_t quantity,
                                      std::uint64_t timestamp) {
    (void)timestamp;

    AddResult out{};
    out.initial_quantity = quantity;

    if (quantity == 0) {
        out.code = ErrorCode::InvalidQuantity;
        return out;
    }



    std::uint64_t remaining = quantity;

    auto match_against = [&](auto& opposite_book) {
        while (remaining > 0 && !opposite_book.empty()) {
            auto level_it = opposite_book.begin();
            auto& queue = level_it->second;

            while (remaining > 0 && !queue.empty()) {
                Order& maker = queue.front();
                const std::uint32_t fill =
                    static_cast<std::uint32_t>(std::min(remaining, maker.quantity));

                out.trades.emplace_back(order_id, maker.id, maker.price, fill);

                maker.quantity -= fill;
                remaining -= fill;
                out.filled_quantity += fill;

                if (maker.quantity == 0) {
                    queue.pop_front();
                }
            }

            if (queue.empty()) {
                opposite_book.erase(level_it);
            }
        }
    };

    if (side == Side::Buy) {
        match_against(asks_);
    } else {
        match_against(bids_);
    }

    out.remaining_quantity = static_cast<std::uint32_t>(remaining);

    if (remaining == 0) {
        out.code = ErrorCode::Success;
        return out;
    }

    out.code = ErrorCode::MarketRemainderCancelled;
    return out;
}

}  // namespace matching
