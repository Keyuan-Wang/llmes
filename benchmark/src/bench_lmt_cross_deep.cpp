/**
 * @file bench_lmt_cross_deep.cpp
 * @brief Benchmark scenario: limit order crossing a deep resting book.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class LmtCrossDeepScenario : public benchmark_runner::IBenchScenario {
    public:
    const char* Name() const override { return "lmt_cross_deep"; }
    // Destructive: each RunOp consumes orders from the book, so every op
    // needs a fresh Setup.
    [[nodiscard]] std::uint64_t max_batch_size() const override { return 1; }

    void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
        book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
        rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
        base_ = 200'000ULL;
        // Fill the sell side at prices 1000, 1001, 1002, ...
        benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, base_);
        safe_base_ = base_ + args.orders + args.levels + 200;
    }

    bool RunOp(const benchmark_runner::Args& args, std::uint64_t,
                 std::uint64_t, std::uint64_t& ok) override {
        const std::uint64_t buy_id = safe_base_ + rng_.next();
        // Price 5000 crosses all levels (asks start at 1000), so the entire
        // sell book at up to args.levels is consumed.
        const auto res = book_->add_limit_order(buy_id, matching::Side::Buy, 5000,
                                                args.levels, buy_id);
        if (res.code == matching::ErrorCode::Success) ++ok;
        return true;
    }

    void Teardown() override { book_.reset(); }

    private:
    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 rng_{42};
    std::uint64_t base_ = 0;
    std::uint64_t safe_base_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    LmtCrossDeepScenario scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
