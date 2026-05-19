/**
 * @file bench_lmt_cross_shallow.cpp
 * @brief Benchmark scenario: limit order crossing a shallow portion of the book.
 *
 * Prefills the sell side with @p orders spread across @p levels, then submits
 * a buy limit order priced to cross only the first 3 price levels. Part of the
 * quantity is filled against resting sells; the remainder rests on the bid
 * side. Measures the partial-match + remainder-insert path, complementing
 * lmt_rest (K=0) and lmt_cross_deep (K=all levels).
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <algorithm>
#include <cstdint>

namespace {

class LmtCrossShallowScenario : public benchmark_runner::IBenchScenario {
 public:
  const char* Name() const override { return "lmt_cross_shallow"; }

  bool PrepareAndRun(const benchmark_runner::Args& args, std::uint64_t op_idx,
                     std::uint64_t& ok) const override {
    matching::OrderBook book(args.orders + args.levels + 100);
    const std::uint64_t base = 700'000ULL + op_idx * 10'000ULL;
    benchmark_runner::PrefillSellBook(book, args.orders, args.levels, base);

    const std::uint64_t per_level =
        std::max<std::uint64_t>(1, args.orders / std::max<std::uint64_t>(1, args.levels));
    const std::uint64_t shallow_depth =
        std::min<std::uint64_t>(3, std::max<std::uint64_t>(1, args.levels));
    const std::uint64_t price = 1000 + shallow_depth - 1;
    const std::uint64_t qty = per_level * shallow_depth + 5;

    const std::uint64_t buy_id = base + args.orders + args.levels + 100;
    const auto res = book.add_limit_order(buy_id, matching::Side::Buy,
                                          static_cast<std::int64_t>(price),
                                          qty,
                                          buy_id);
    if (res.code == matching::ErrorCode::Success) ++ok;
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  return benchmark_runner::RunScenario(LmtCrossShallowScenario{}, argc, argv);
}
