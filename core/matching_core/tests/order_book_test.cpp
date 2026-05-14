#include "matching/order_book.hpp"

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
    matching::OrderBook book;

    // 1. push limit order to an empty book: only insert the orders but no matching
    {
        const auto r = book.add_limit_order(1, matching::Side::Buy, 100, 10, 1);
        expect(r.code == matching::ErrorCode::Success, "Limite rest success");
        expect(r.filled_quantity == 0, "no fill");
        expect(r.remaining_quantity == 10, "full qty rests");
        expect(r.trades.empty(), "no trade");
    }

    // 2. matching + push remaining orders in the book
    {
        matching::OrderBook b;
        (void)b.add_limit_order(10, matching::Side::Sell, 100, 5, 1);
        const auto r = b.add_limit_order(11, matching::Side::Buy, 100, 12, 2);
        expect(r.code == matching::ErrorCode::Success, "cross Success");
        expect(r.filled_quantity == 5, "filled 5");
        expect(r.remaining_quantity == 7, "rest 7");
        expect(r.trades.size() == 1, "one trade");
        expect(r.trades[0].maker_order_id == 10, "maker id");
        expect(r.trades[0].quantity == 5, "trade qty");
      }

    // 3. market orders wipe all orders, reamining orders are canceled
    {
        matching::OrderBook b;
        (void)b.add_limit_order(101, matching::Side::Sell, 100, 5, 1);
        (void)b.add_limit_order(102, matching::Side::Sell, 101, 5, 2);
        const auto r = b.add_market_order(500, matching::Side::Buy, 15, 3);
        expect(r.filled_quantity == 10, "market fill 10");
        expect(r.remaining_quantity == 5, "remainder 5");
        expect(r.code == matching::ErrorCode::MarketRemainderCancelled, "remainder cancelled");
    }

    // 4. pending cancel + duplicate / pending insert
    {
        matching::OrderBook b;
        expect(b.cancel_order(42) == matching::ErrorCode::UnknownOrderId, "unknown cancel");
        expect(b.pending_cancel_count() == 1, "pending size");
        const auto r = b.add_limit_order(42, matching::Side::Buy, 100, 1, 1);
        expect(r.code == matching::ErrorCode::PendingCancelExists, "blocked by pending cancel");
    }

    std::cout << "order_book tests passed\n";
    return 0;
}