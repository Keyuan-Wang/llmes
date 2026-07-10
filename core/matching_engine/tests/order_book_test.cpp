#include "llme/matching/order_book.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL : " << msg << "\n";
        std::exit(1);
    }
}
}

int main() {
    llme::matching::OrderBook book;

    // 1. push limit order to an empty book: only insert the orders but no matching
    {
        const auto r = book.add_limit_order(1, llme::matching::Side::Buy, 100, 10, 1);
        expect(r.code == llme::matching::ErrorCode::Success, "Limite rest success");
        expect(r.filled_quantity == 0, "no fill");
        expect(r.remaining_quantity == 10, "full qty rests");
        expect(r.trades.empty(), "no trade");
        expect(r.handle != llme::matching::kInvalidHandle, "resting order returns handle");
    }

    // 2. matching + push remaining orders in the book
    {
        llme::matching::OrderBook b;
        (void)b.add_limit_order(10, llme::matching::Side::Sell, 100, 5, 1);
        const auto r = b.add_limit_order(11, llme::matching::Side::Buy, 100, 12, 2);
        expect(r.code == llme::matching::ErrorCode::Success, "cross Success");
        expect(r.filled_quantity == 5, "filled 5");
        expect(r.remaining_quantity == 7, "rest 7");
        expect(r.trades.size() == 1, "one trade");
        expect(r.trades[0].maker_order_id == 10, "maker id");
        expect(r.trades[0].quantity == 5, "trade qty");
      }

    // 3. market orders wipe all orders, reamining orders are canceled
    {
        llme::matching::OrderBook b;
        (void)b.add_limit_order(101, llme::matching::Side::Sell, 100, 5, 1);
        (void)b.add_limit_order(102, llme::matching::Side::Sell, 101, 5, 2);
        const auto r = b.add_market_order(500, llme::matching::Side::Buy, 15, 3);
        expect(r.filled_quantity == 10, "market fill 10");
        expect(r.remaining_quantity == 5, "remainder 5");
        expect(r.code == llme::matching::ErrorCode::MarketRemainderCancelled, "remainder cancelled");
    }

    // 4. handle-based cancel
    {
        llme::matching::OrderBook b;
        const auto r = b.add_limit_order(42, llme::matching::Side::Buy, 100, 1, 1);
        expect(r.code == llme::matching::ErrorCode::Success, "insert before cancel");
        expect(r.handle != llme::matching::kInvalidHandle, "cancel target handle");
        expect(b.cancel_order(r.handle) == llme::matching::ErrorCode::Success, "handle cancel");
    }

    // 5. business order ids are not used for matching-core uniqueness checks
    {
        llme::matching::OrderBook b;
        const auto first = b.add_limit_order(7, llme::matching::Side::Buy, 100, 10, 1);
        expect(first.code == llme::matching::ErrorCode::Success, "first insert ok");

        const auto second = b.add_limit_order(7, llme::matching::Side::Buy, 99, 5, 2);
        expect(second.code == llme::matching::ErrorCode::Success, "duplicate business id allowed");
        expect(second.handle != first.handle, "each resting order has its own handle");
    }

    std::cout << "order_book tests passed\n";
    return 0;
}