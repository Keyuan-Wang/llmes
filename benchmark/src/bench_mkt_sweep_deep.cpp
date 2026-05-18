/**
 * @file bench_mkt_sweep_deep.cpp
 * @brief Benchmark scenario: market order sweeping a deep resting book.
 *
 * Prefills the sell side with @p orders spread across @p levels, then
 * submits an aggressive buy market order for quantity = @p levels * 2.
 * The market order consumes as many resting sells as possible; any unfilled
 * remainder is cancelled. Measures the cost of sequential matching +
 * queue removal across multiple price levels.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

namespace {

class MktSweepDeepScenario : public benchmark_runner::IBenchScenario {
 public:
  const char* Name() const override { return "mkt_sweep_deep"; }

  bool PrepareAndRun(const benchmark_runner::Args& args, std::uint64_t op_idx,
                     std::uint64_t& ok) const override {
    matching::OrderBook book;
    const std::uint64_t base = 300'000ULL + op_idx * 10'000ULL;
    benchmark_runner::PrefillSellBook(book, args.orders, args.levels, base);
    const std::uint64_t mkt_id = base + args.orders + args.levels + 100;
    const auto res = book.add_market_order(mkt_id, matching::Side::Buy,
                                           args.levels * 2,
                                           mkt_id);
    if (res.code == matching::ErrorCode::Success ||
        res.code == matching::ErrorCode::MarketRemainderCancelled) {
      ++ok;
    }
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  return benchmark_runner::RunScenario(MktSweepDeepScenario{}, argc, argv);
}
