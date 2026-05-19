/**
 * @file bench_cxl_hit.cpp
 * @brief Benchmark scenario: cancel an existing order (successful cancel).
 *
 * Prefills the sell side with @p orders spread across @p levels, then cancels
 * the first order ID that was inserted — the order is guaranteed to exist.
 * Measures the successful cancel hot path. Paired with bench_cxl_miss.cpp
 * (cancel non-existent ID) for complete cancel-path coverage.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

namespace {

class CxlHitScenario : public benchmark_runner::IBenchScenario {
 public:
  const char* Name() const override { return "cxl_hit"; }

  bool PrepareAndRun(const benchmark_runner::Args& args, std::uint64_t op_idx,
                     std::uint64_t& ok) const override {
    matching::OrderBook book(args.orders + args.levels + 100);
    const std::uint64_t base = 600'000ULL + op_idx * 10'000ULL;
    benchmark_runner::PrefillSellBook(book, args.orders, args.levels, base);
    const auto code = book.cancel_order(base);
    if (code == matching::ErrorCode::Success) ++ok;
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  return benchmark_runner::RunScenario(CxlHitScenario{}, argc, argv);
}
