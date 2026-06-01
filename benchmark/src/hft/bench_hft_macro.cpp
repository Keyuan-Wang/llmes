/**
 * @file bench_hft_macro.cpp
 * @brief HFT macro benchmark: Zero-Intelligence model of HFT order flow.
 *
 * Generates a continuous stream of empirically-grounded order-book events:
 *   45% limit add (near best price), 48% cancel, 5% modify, 2% market.
 * Spontaneous cancel clusters (power-law size, 15% trigger probability)
 * reproduce the temporal autocorrelation observed in real HFT markets.
 *
 * THE KEY DESIGN CHOICE:
 * All event parameters (RNG, cancel-target selection, tracking-map updates)
 * are pre-generated in Setup() -- which is outside the timed window.
 * RunOp() executes pure OrderBook operations only, so latency/PMC samples
 * measure only the engine, not the benchmark harness.
 *
 * Reference: Gode & Sunder (1993), "Allocative Efficiency of Markets with
 * Zero-Intelligence Traders." JPE 101(1), 119-137.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include "absl/container/flat_hash_map.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef LLMES_PROFILE_HFT_MACRO_OPS
#define LLMES_PROFILE_HFT_MACRO_OPS 0
#endif

#if LLMES_PROFILE_HFT_MACRO_OPS && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#endif

namespace {

// ------------------------------------------------------------------
//  Pre-generated book-operation parameters.
//  One PendingOp per RunOp() call -- generated in Setup(), consumed in
//  RunOp(), so RNG and map lookups are outside the timed window.
// ------------------------------------------------------------------
struct PendingOp {
    enum Type : std::uint8_t { kLimitAdd, kCancel, kModify, kMarket };
    Type type;

    // kLimitAdd / kModify
    matching::Side side;
    std::int64_t price;
    std::uint64_t qty;

    // kCancel / kModify
    std::uint64_t target_id;

    // kMarket
    std::uint64_t market_qty;

    // All
    std::uint64_t oid;
};

#if LLMES_PROFILE_HFT_MACRO_OPS
template <typename T>
double QuantileNearestRank(std::vector<T> values, double q) {
    if (values.empty()) return 0.0;
    const std::size_t idx = static_cast<std::size_t>(
        q * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + idx, values.end());
    return static_cast<double>(values[idx]);
}
#endif

// ------------------------------------------------------------------
class BenchHftMacro final : public benchmark_runner::IBenchScenario {
public:
    const char* Name() const override { return "hft_macro"; }
    [[nodiscard]] std::uint64_t max_batch_size() const override {
        return 1'000'000;
    }

    void Setup(const benchmark_runner::Args& args,
               std::uint64_t iter_idx) override {
        const std::uint64_t warmup_events = 500'000;
        const std::uint64_t pool = warmup_events + args.batch_size + 50000;
        const std::uint64_t max_active_levels = std::min(pool, kPriceRangeLevels);

        book_ = std::make_unique<matching::OrderBook>(pool, max_active_levels);
        event_rng_ =
            benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
        param_rng_ = benchmark_runner::SplitMix64(
            args.seed * 1337ULL + iter_idx * 331ULL);
        id_counter_ = 1'000'000ULL;
        best_bid_ = kInitialBestBid;
        best_ask_ = kInitialBestAsk;

#if LLMES_PROFILE_HFT_MACRO_OPS
        current_iter_is_measured_ = (iter_idx >= args.warmup_iters);
        if (iter_idx == 0) {
            ResetProfileState(args);
        }
#endif

        // --- Warmup (untimed, uses the old generate-execute-track loop) ---
        resting_orders_.clear();
        resting_ids_.clear();
        level_counts_.clear();
        cluster_queue_.clear();

        // Initial seed: a few hundred adds for depth
        for (std::uint64_t i = 0; i < 5000; ++i) {
            do_limit_add();
        }

        for (std::uint64_t i = 0; i < warmup_events; ++i) {
            generate_and_execute_one();
        }

        update_best_prices();

        // --- Pre-generate the measured batch ---
        // All event params are decided now.  RunOp() just replays them on
        // the book -- no RNG, no map lookups, no tracking updates inside
        // the timed window.
        pending_.clear();
        pregen_queue_.clear();
        while (pending_.size() < args.batch_size) {
            generate_pending_one();
        }
        pending_.resize(args.batch_size);  // discard excess from clusters
    }

    bool RunOp(const benchmark_runner::Args& args, std::uint64_t iter_idx,
               std::uint64_t batch_idx, std::uint64_t& ok) override {
#if LLMES_PROFILE_HFT_MACRO_OPS
        if (iter_idx >= args.warmup_iters) {
            const std::uint64_t c0 = ReadCycles();
            const auto t0 = Clock::now();
            const ExecuteOutcome outcome = execute_pending(batch_idx, ok);
            const auto t1 = Clock::now();
            const std::uint64_t c1 = ReadCycles();

            const auto dt_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                t1 - t0).count();
            const std::uint64_t cycles = (c1 >= c0) ? (c1 - c0) : 0ULL;
            RecordProfileSample(outcome, static_cast<double>(dt_ns), cycles);
            return true;
        }
#endif
        (void)execute_pending(batch_idx, ok);
        return true;
    }

    void Teardown() override {
        book_.reset();
#if LLMES_PROFILE_HFT_MACRO_OPS
        if (current_iter_is_measured_) {
            ++measured_iters_done_;
            if (!profile_emitted_ && measured_iters_done_ >= profile_args_.iters) {
                EmitProfile();
                profile_emitted_ = true;
            }
        }
#endif
    }

private:
    // Fixed absolute price band for the zero-intelligence macro workload.
    // Because crossing orders immediately trade, live bid/ask price levels
    // cannot overlap; therefore the live level count is bounded by this range.
    static constexpr std::int64_t kMinPrice = 1;
    static constexpr std::int64_t kMaxPrice = 8192;
    static constexpr std::int64_t kInitialBestBid = 999;
    static constexpr std::int64_t kInitialBestAsk = 1000;
    static constexpr std::uint64_t kPriceRangeLevels =
        static_cast<std::uint64_t>(kMaxPrice - kMinPrice + 1);
    enum class OpBucket : std::uint8_t {
        kAddRest = 0,
        kAddCross,
        kCancelHit,
        kCancelMiss,
        kModifyHit,
        kModifyMiss,
        kMarket,
        kCount
    };

    struct ExecuteOutcome {
        OpBucket bucket = OpBucket::kMarket;
        std::uint64_t market_levels_touched = 0;
        std::uint64_t market_filled_qty = 0;
    };

#if LLMES_PROFILE_HFT_MACRO_OPS
    using Clock = std::chrono::steady_clock;

    struct OpProfile {
        const char* name = "";
        std::uint64_t count = 0;
        long double total_ns = 0.0L;
        std::uint64_t total_cycles = 0;
        std::vector<double> ns_samples;
        std::uint64_t market_levels_sum = 0;
        std::uint64_t market_filled_sum = 0;
        std::vector<std::uint64_t> market_levels_samples;
        std::vector<std::uint64_t> market_filled_samples;
    };
#endif

    // ================================================================
    //  State
    // ================================================================

    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 event_rng_{42};
    benchmark_runner::SplitMix64 param_rng_{42};
    std::uint64_t id_counter_ = 0;

    // Resting-order tracking (used for cancel-target selection during the
    // pre-generation step in Setup, NOT during RunOp).
    absl::flat_hash_map<std::uint64_t, std::int64_t> resting_orders_;
    std::vector<std::uint64_t> resting_ids_;
    absl::flat_hash_map<std::int64_t, std::uint64_t> level_counts_;

    std::int64_t best_bid_ = 0;
    std::int64_t best_ask_ = kInitialBestAsk;

    // Cluster cancel ID queue (for warmup's generate-and-execute path)
    std::vector<std::uint64_t> cluster_queue_;

    // Pre-generated event parameters for the timed batch
    std::vector<PendingOp> pending_;
    // Overflow queue: cluster cancels that didn't fit in pending_ spill here
    std::vector<PendingOp> pregen_queue_;

#if LLMES_PROFILE_HFT_MACRO_OPS
    std::array<OpProfile, static_cast<std::size_t>(OpBucket::kCount)> op_profile_{};
    benchmark_runner::Args profile_args_{};
    std::uint64_t measured_iters_done_ = 0;
    bool current_iter_is_measured_ = false;
    bool profile_emitted_ = false;
#endif

    static bool price_in_range(std::int64_t price) noexcept {
        return price >= kMinPrice && price <= kMaxPrice;
    }

    // Reflecting keeps out-of-range samples inside the fixed price band without
    // piling all boundary hits onto kMinPrice/kMaxPrice the way clamp() would.
    static std::int64_t reflect_price(std::int64_t price) noexcept {
        if (price_in_range(price)) {
            return price;
        }

        constexpr std::int64_t span = kMaxPrice - kMinPrice;
        constexpr std::int64_t period = 2 * span;

        std::int64_t offset = (price - kMinPrice) % period;
        if (offset < 0) {
            offset += period;
        }

        return (offset <= span)
            ? (kMinPrice + offset)
            : (kMaxPrice - (offset - span));
    }

    int draw_near_best_offset() {
        int offset = 0;
        std::uint64_t r = param_rng_.next();
        while ((r & 0xFF) < 161 && offset < 100) {
            ++offset;
            r >>= 8;
        }
        return (offset == 0) ? 1 : offset;
    }

    std::int64_t bounded_best_ask_reference() const noexcept {
        return reflect_price((best_ask_ > 0) ? best_ask_ : kInitialBestAsk);
    }

    std::int64_t draw_limit_price(matching::Side side) {
        const std::int64_t ref = bounded_best_ask_reference();
        std::int64_t candidate = ref;

        for (int attempt = 0; attempt < 8; ++attempt) {
            const int offset = draw_near_best_offset();
            candidate = (side == matching::Side::Buy)
                ? ref - offset - 1
                : ref + offset;
            if (price_in_range(candidate)) {
                return candidate;
            }
        }

        return reflect_price(candidate);
    }

    std::int64_t draw_modified_price(std::int64_t old_price) {
        const int delta = 1 + static_cast<int>(param_rng_.next() % 3);
        const std::int64_t candidate =
            old_price + ((param_rng_.next() % 2 == 0) ? -delta : delta);

        return price_in_range(candidate) ? candidate : reflect_price(candidate);
    }

    // ================================================================
    //  Warmup event generation and execution (legacy path, untimed)
    // ================================================================

    void generate_and_execute_one() {
        if (!cluster_queue_.empty()) {
            std::uint64_t target_id = cluster_queue_.back();
            cluster_queue_.pop_back();
            if (do_cancel(target_id)) return;
        }

        std::uint64_t const roll = event_rng_.next() % 100;
        if (roll < 45) { do_limit_add(); return; }
        if (roll < 93) { do_cancel_random(); return; }
        if (roll < 98) { do_modify(); return; }
        do_market();
    }

    // ================================================================
    //  Pre-generation for the timed batch (in Setup, untimed)
    // ================================================================

    void generate_pending_one() {
        // Drain pregen queue first
        if (!pregen_queue_.empty()) {
            pending_.push_back(pregen_queue_.back());
            pregen_queue_.pop_back();
            return;
        }

        std::uint64_t const roll = event_rng_.next() % 100;
        if (roll < 45) { pending_limit_add(); return; }
        if (roll < 93) { pending_cancel(); return; }
        if (roll < 98) { pending_modify(); return; }
        pending_market();
    }

    // --- Limit add pre-gen ---
    void pending_limit_add() {
        PendingOp op;
        op.type = PendingOp::kLimitAdd;
        op.side = (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                                : matching::Side::Sell;

        op.price = draw_limit_price(op.side);

        static constexpr std::uint64_t kQtyTable[32] = {
            1,1,1,2,2,2,3,3,3,4,4,4,5,5,5,5,
            5,6,6,6,7,7,8,9,10,12,15,20,30,50,75,100};
        op.qty = kQtyTable[param_rng_.next() % 32];
        op.oid = id_counter_++;

        // Predictive tracking update (assuming success)
        track_add_predicted(op.oid, op.price);
        if (op.side == matching::Side::Buy && op.price > best_bid_)
            best_bid_ = op.price;
        if (op.side == matching::Side::Sell &&
            (best_ask_ == 0 || op.price < best_ask_))
            best_ask_ = op.price;

        pending_.push_back(op);
    }

    // --- Cancel pre-gen ---
    void pending_cancel() {
        if (resting_orders_.empty()) { pending_limit_add(); return; }

        std::uint64_t const target = select_cancel_target();
        if (target == 0) { pending_limit_add(); return; }

        PendingOp op;
        op.type = PendingOp::kCancel;
        op.target_id = target;
        op.oid = 0;
        pending_.push_back(op);

        // Predictive tracking update
        auto const it = resting_orders_.find(target);
        if (it != resting_orders_.end()) {
            track_remove_predicted(target, it->second);
        }

        // Cluster generation: push extra cancel ops into pregen_queue_
        if ((param_rng_.next() % 100) < 15) {
            enqueue_cluster_pending();
        }
    }

    // --- Modify pre-gen ---
    void pending_modify() {
        if (resting_orders_.empty()) { pending_limit_add(); return; }

        std::uint64_t const target = select_cancel_target();
        if (target == 0) { pending_limit_add(); return; }

        auto const it = resting_orders_.find(target);
        std::int64_t const old_price =
            (it != resting_orders_.end()) ? it->second : 0;

        std::int64_t const new_price = draw_modified_price(old_price);
        std::uint64_t const new_qty = 1 + (param_rng_.next() % 20);

        PendingOp op;
        op.type = PendingOp::kModify;
        op.side = (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                                : matching::Side::Sell;
        op.price = new_price;
        op.qty = new_qty;
        op.target_id = target;
        op.oid = id_counter_++;
        pending_.push_back(op);

        // Predictive tracking update
        if (old_price > 0) track_remove_predicted(target, old_price);
        track_add_predicted(target, new_price);
        if (op.side == matching::Side::Buy && new_price > best_bid_)
            best_bid_ = new_price;
        if (op.side == matching::Side::Sell &&
            (best_ask_ == 0 || new_price < best_ask_))
            best_ask_ = new_price;
    }

    // --- Market order pre-gen ---
    void pending_market() {
        std::uint64_t const total = resting_orders_.size();
        if (total == 0) { pending_limit_add(); return; }

        PendingOp op;
        op.type = PendingOp::kMarket;
        op.side = (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                                : matching::Side::Sell;

        int const mroll = static_cast<int>(param_rng_.next() % 100);
        if (mroll < 85)
            op.market_qty = std::max<std::uint64_t>(1, total / 4);
        else if (mroll < 98)
            op.market_qty = std::max<std::uint64_t>(1, total / 2);
        else
            op.market_qty = static_cast<std::uint64_t>(total * 0.8);

        op.oid = id_counter_++;
        pending_.push_back(op);

        // We cannot predict which specific maker orders will be filled by
        // the market order (the engine decides based on FIFO queue order).
        // Skip tracking updates -- the tracking will be slightly optimistic
        // for remaining events in this batch, but the error is negligible
        // (market is 2% of events) and resets every iteration.
    }

    // ================================================================
    //  Timed execution -- RunOp calls this, pure book ops only
    // ================================================================

    ExecuteOutcome execute_pending(std::size_t idx, std::uint64_t& ok) {
        ExecuteOutcome outcome{};

        auto const& op = pending_[idx];
        switch (op.type) {
        case PendingOp::kLimitAdd: {
            auto const res =
                book_->add_limit_order(op.oid, op.side, op.price, op.qty, op.oid);
            if (res.code == matching::ErrorCode::Success) ++ok;
            outcome.bucket =
                (res.code == matching::ErrorCode::Success && res.remaining_quantity > 0)
                    ? OpBucket::kAddRest
                    : OpBucket::kAddCross;
            break;
        }
        case PendingOp::kCancel: {
            auto const code = book_->cancel_order(op.target_id);
            if (code == matching::ErrorCode::Success) {
                ++ok;
                outcome.bucket = OpBucket::kCancelHit;
            } else {
                outcome.bucket = OpBucket::kCancelMiss;
            }
            break;
        }
        case PendingOp::kModify: {
            const bool existed_before = book_->contains_order(op.target_id);
            auto const res = book_->modify_order(op.target_id, op.side, op.price,
                                                 op.qty, op.oid);
            if (res.code == matching::ErrorCode::Success) ++ok;
            outcome.bucket =
                existed_before ? OpBucket::kModifyHit : OpBucket::kModifyMiss;
            break;
        }
        case PendingOp::kMarket: {
            auto const res =
                book_->add_market_order(op.oid, op.side, op.market_qty, op.oid);
            if (res.code == matching::ErrorCode::Success ||
                res.code == matching::ErrorCode::MarketRemainderCancelled)
                ++ok;

            outcome.bucket = OpBucket::kMarket;
            outcome.market_filled_qty = res.filled_quantity;
            outcome.market_levels_touched = CountTouchedLevels(res.trades);
            break;
        }
        }

        return outcome;
    }

    static std::uint64_t CountTouchedLevels(
        const std::vector<matching::Trade>& trades) noexcept {
        if (trades.empty()) return 0;

        std::uint64_t levels = 0;
        std::int64_t last_price = 0;
        bool has_last = false;
        for (const auto& trade : trades) {
            if (!has_last || trade.price != last_price) {
                ++levels;
                last_price = trade.price;
                has_last = true;
            }
        }
        return levels;
    }

#if LLMES_PROFILE_HFT_MACRO_OPS
    static std::uint64_t ReadCycles() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        return __rdtsc();
#else
        return 0;
#endif
    }

    void ResetProfileState(const benchmark_runner::Args& args) {
        profile_args_ = args;
        measured_iters_done_ = 0;
        profile_emitted_ = false;

        static constexpr std::array<const char*, static_cast<std::size_t>(OpBucket::kCount)>
            kNames = {
                "add_rest",
                "add_cross",
                "cancel_hit",
                "cancel_miss",
                "modify_hit",
                "modify_miss",
                "market"
            };

        const std::size_t samples = static_cast<std::size_t>(args.batch_size * args.iters);
        const std::size_t reserve_total = std::max<std::size_t>(samples, 64);
        const std::array<std::size_t, static_cast<std::size_t>(OpBucket::kCount)> weights = {
            30, 15, 40, 8, 4, 1, 2
        };

        for (std::size_t i = 0; i < op_profile_.size(); ++i) {
            auto& p = op_profile_[i];
            p.name = kNames[i];
            p.count = 0;
            p.total_ns = 0.0L;
            p.total_cycles = 0;
            p.market_levels_sum = 0;
            p.market_filled_sum = 0;
            p.ns_samples.clear();
            p.market_levels_samples.clear();
            p.market_filled_samples.clear();

            const std::size_t reserve =
                std::max<std::size_t>(32, (reserve_total * weights[i]) / 100);
            p.ns_samples.reserve(reserve);
            if (i == static_cast<std::size_t>(OpBucket::kMarket)) {
                p.market_levels_samples.reserve(reserve);
                p.market_filled_samples.reserve(reserve);
            }
        }
    }

    void RecordProfileSample(const ExecuteOutcome& outcome, double ns,
                             std::uint64_t cycles) {
        auto& p = op_profile_[static_cast<std::size_t>(outcome.bucket)];
        ++p.count;
        p.total_ns += ns;
        p.total_cycles += cycles;
        p.ns_samples.push_back(ns);

        if (outcome.bucket == OpBucket::kMarket) {
            p.market_levels_sum += outcome.market_levels_touched;
            p.market_filled_sum += outcome.market_filled_qty;
            p.market_levels_samples.push_back(outcome.market_levels_touched);
            p.market_filled_samples.push_back(outcome.market_filled_qty);
        }
    }

    void EmitProfile() const {
        std::uint64_t total_ops = 0;
        long double total_ns = 0.0L;
        long double total_cycles = 0.0L;
        for (const auto& p : op_profile_) {
            total_ops += p.count;
            total_ns += p.total_ns;
            total_cycles += static_cast<long double>(p.total_cycles);
        }

        if (total_ops == 0) return;

        const char* out_path = std::getenv("LLMES_HFT_MACRO_OP_PROFILE_OUT");
        const bool write_csv = (out_path != nullptr && out_path[0] != '\0');
        if (write_csv) {
            benchmark_runner::EnsureCsvHeader(
                out_path,
                "scenario,version_tag,commit_sha,trial_id,seed,orders,levels,batch_size,"
                "warmup_iters,iters,op_type,count,share,mean_ns,p50_ns,p95_ns,p99_ns,"
                "mean_cycles,weighted_ns_per_event,weighted_cycles_per_event,"
                "weighted_ns_share,weighted_cycles_share,market_levels_mean,"
                "market_levels_p95,market_levels_p99,market_filled_qty_mean,"
                "market_filled_qty_p95,market_filled_qty_p99");
        }

        std::cout << "macro_op_profile"
                  << " scenario=" << Name()
                  << " version_tag=" << profile_args_.version_tag
                  << " commit_sha=" << profile_args_.commit_sha
                  << " trial_id=" << profile_args_.trial_id
                  << " seed=" << profile_args_.seed
                  << " orders=" << profile_args_.orders
                  << " levels=" << profile_args_.levels
                  << " batch_size=" << profile_args_.batch_size
                  << " warmup_iters=" << profile_args_.warmup_iters
                  << " iters=" << profile_args_.iters
                  << " total_ops=" << total_ops
                  << "\n";

        for (const auto& p : op_profile_) {
            const double share = static_cast<double>(p.count) /
                                 static_cast<double>(total_ops);
            const double mean_ns = (p.count > 0)
                                       ? static_cast<double>(p.total_ns /
                                            static_cast<long double>(p.count))
                                       : 0.0;
            const double p50_ns = benchmark_runner::Percentile(p.ns_samples, 0.50);
            const double p95_ns = benchmark_runner::Percentile(p.ns_samples, 0.95);
            const double p99_ns = benchmark_runner::Percentile(p.ns_samples, 0.99);
            const double mean_cycles = (p.count > 0)
                                           ? static_cast<double>(p.total_cycles) /
                                                 static_cast<double>(p.count)
                                           : 0.0;

            const double weighted_ns_per_event = share * mean_ns;
            const double weighted_cycles_per_event = share * mean_cycles;
            const double weighted_ns_share =
                (total_ns > 0.0L)
                    ? static_cast<double>(p.total_ns / total_ns)
                    : 0.0;
            const double weighted_cycles_share =
                (total_cycles > 0.0L)
                    ? static_cast<double>(
                        static_cast<long double>(p.total_cycles) / total_cycles)
                    : 0.0;

            double market_levels_mean = 0.0;
            double market_levels_p95 = 0.0;
            double market_levels_p99 = 0.0;
            double market_filled_mean = 0.0;
            double market_filled_p95 = 0.0;
            double market_filled_p99 = 0.0;
            if (std::string(p.name) == "market" && p.count > 0) {
                market_levels_mean = static_cast<double>(p.market_levels_sum) /
                                     static_cast<double>(p.count);
                market_levels_p95 =
                    QuantileNearestRank(p.market_levels_samples, 0.95);
                market_levels_p99 =
                    QuantileNearestRank(p.market_levels_samples, 0.99);
                market_filled_mean = static_cast<double>(p.market_filled_sum) /
                                     static_cast<double>(p.count);
                market_filled_p95 =
                    QuantileNearestRank(p.market_filled_samples, 0.95);
                market_filled_p99 =
                    QuantileNearestRank(p.market_filled_samples, 0.99);
            }

            std::cout << "macro_op_profile"
                      << " op_type=" << p.name
                      << " count=" << p.count
                      << " share=" << share
                      << " mean_ns=" << mean_ns
                      << " p50_ns=" << p50_ns
                      << " p95_ns=" << p95_ns
                      << " p99_ns=" << p99_ns
                      << " mean_cycles=" << mean_cycles
                      << " weighted_ns_per_event=" << weighted_ns_per_event
                      << " weighted_cycles_per_event=" << weighted_cycles_per_event
                      << " weighted_ns_share=" << weighted_ns_share
                      << " weighted_cycles_share=" << weighted_cycles_share
                      << " market_levels_mean=" << market_levels_mean
                      << " market_levels_p95=" << market_levels_p95
                      << " market_levels_p99=" << market_levels_p99
                      << " market_filled_qty_mean=" << market_filled_mean
                      << " market_filled_qty_p95=" << market_filled_p95
                      << " market_filled_qty_p99=" << market_filled_p99
                      << "\n";

            if (write_csv) {
                std::ofstream f(out_path, std::ios::app);
                f << Name()
                  << ","
                  << profile_args_.version_tag
                  << ","
                  << profile_args_.commit_sha
                  << ","
                  << profile_args_.trial_id
                  << ","
                  << profile_args_.seed
                  << ","
                  << profile_args_.orders
                  << ","
                  << profile_args_.levels
                  << ","
                  << profile_args_.batch_size
                  << ","
                  << profile_args_.warmup_iters
                  << ","
                  << profile_args_.iters
                  << ","
                  << p.name
                  << ","
                  << p.count
                  << ","
                  << share
                  << ","
                  << mean_ns
                  << ","
                  << p50_ns
                  << ","
                  << p95_ns
                  << ","
                  << p99_ns
                  << ","
                  << mean_cycles
                  << ","
                  << weighted_ns_per_event
                  << ","
                  << weighted_cycles_per_event
                  << ","
                  << weighted_ns_share
                  << ","
                  << weighted_cycles_share
                  << ","
                  << market_levels_mean
                  << ","
                  << market_levels_p95
                  << ","
                  << market_levels_p99
                  << ","
                  << market_filled_mean
                  << ","
                  << market_filled_p95
                  << ","
                  << market_filled_p99
                  << "\n";
            }
        }
    }
#endif

    // ================================================================
    //  Tracking helpers -- only called during warmup + pre-generation
    // ================================================================

    void track_add_predicted(std::uint64_t id, std::int64_t price) {
        resting_orders_[id] = price;
        resting_ids_.push_back(id);
        ++level_counts_[price];
    }

    void track_remove_predicted(std::uint64_t id, std::int64_t price) {
        resting_orders_.erase(id);
        auto lc = level_counts_.find(price);
        if (lc != level_counts_.end() && lc->second > 0) {
            --lc->second;
            if (lc->second == 0) level_counts_.erase(lc);
        }
    }

    void update_best_prices() {
        best_bid_ = 0;
        best_ask_ = 0;
        for (auto const& [price, count] : level_counts_) {
            if (count == 0) continue;
            if (price >= kInitialBestAsk) {
                if (best_ask_ == 0 || price < best_ask_) best_ask_ = price;
            } else {
                if (best_bid_ == 0 || price > best_bid_) best_bid_ = price;
            }
        }
        if (best_ask_ == 0) best_ask_ = kInitialBestAsk;
    }

    // ================================================================
    //  Legacy warmup do_* methods (generate + execute + track inline)
    // ================================================================

    bool do_limit_add() {
        matching::Side const side =
            (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                         : matching::Side::Sell;
        std::int64_t const price = draw_limit_price(side);
        static constexpr std::uint64_t kQtyTable[32] = {
            1,1,1,2,2,2,3,3,3,4,4,4,5,5,5,5,
            5,6,6,6,7,7,8,9,10,12,15,20,30,50,75,100};
        std::uint64_t const qty = kQtyTable[param_rng_.next() % 32];
        std::uint64_t const oid = id_counter_++;
        auto const res =
            book_->add_limit_order(oid, side, price, qty, oid);
        if (res.code == matching::ErrorCode::Success && res.remaining_quantity > 0) {
            track_add_predicted(oid, price);
            if (side == matching::Side::Buy && price > best_bid_) best_bid_ = price;
            if (side == matching::Side::Sell &&
                (best_ask_ == 0 || price < best_ask_))
                best_ask_ = price;
            return true;
        }
        return (res.code == matching::ErrorCode::Success);
    }

    bool do_cancel_random() {
        if (resting_orders_.empty()) return do_limit_add();
        std::uint64_t const target = select_cancel_target();
        if (target == 0) return do_limit_add();
        return do_cancel(target);
    }

    bool do_cancel(std::uint64_t target_id) {
        auto const code = book_->cancel_order(target_id);
        if (code == matching::ErrorCode::Success) {
            auto const it = resting_orders_.find(target_id);
            if (it != resting_orders_.end()) {
                track_remove_predicted(target_id, it->second);
            }
            if ((param_rng_.next() % 100) < 15) {
                enqueue_cluster_legacy();
            }
            return true;
        }
        return false;
    }

    bool do_modify() {
        if (resting_orders_.empty()) return do_limit_add();
        std::uint64_t const target = select_cancel_target();
        if (target == 0) return do_limit_add();
        auto const it = resting_orders_.find(target);
        std::int64_t const old_price =
            (it != resting_orders_.end()) ? it->second : 0;
        std::int64_t const new_price = draw_modified_price(old_price);
        std::uint64_t const new_qty = 1 + (param_rng_.next() % 20);
        std::uint64_t const ts = id_counter_++;
        matching::Side const side =
            (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                         : matching::Side::Sell;
        auto const res =
            book_->modify_order(target, side, new_price, new_qty, ts);
        if (res.code == matching::ErrorCode::Success) {
            if (old_price > 0) track_remove_predicted(target, old_price);
            if (res.remaining_quantity > 0) {
                track_add_predicted(target, new_price);
                if (side == matching::Side::Buy && new_price > best_bid_)
                    best_bid_ = new_price;
                if (side == matching::Side::Sell &&
                    (best_ask_ == 0 || new_price < best_ask_))
                    best_ask_ = new_price;
            }
            return true;
        }
        return false;
    }

    bool do_market() {
        std::uint64_t const total = resting_orders_.size();
        if (total == 0) return do_limit_add();
        matching::Side const side =
            (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                         : matching::Side::Sell;
        int const mroll = static_cast<int>(param_rng_.next() % 100);
        std::uint64_t qty;
        if (mroll < 85)
            qty = std::max<std::uint64_t>(1, total / 4);
        else if (mroll < 98)
            qty = std::max<std::uint64_t>(1, total / 2);
        else
            qty = static_cast<std::uint64_t>(total * 0.8);
        std::uint64_t const oid = id_counter_++;
        auto const res =
            book_->add_market_order(oid, side, qty, oid);
        if (res.code == matching::ErrorCode::Success ||
            res.code == matching::ErrorCode::MarketRemainderCancelled) {
            for (auto const& trade : res.trades) {
                auto const rit = resting_orders_.find(trade.maker_order_id);
                if (rit != resting_orders_.end()) {
                    track_remove_predicted(trade.maker_order_id, rit->second);
                }
            }
            update_best_prices();
            return true;
        }
        return false;
    }

    // ================================================================
    //  Cancel target selection (60/30/10 zone distribution)
    // ================================================================

    std::uint64_t select_cancel_target() {
        if (resting_orders_.empty()) return 0;

        int const zroll = static_cast<int>(param_rng_.next() % 100);
        int min_dist, max_dist;
        if (zroll < 60) {
            min_dist = 0; max_dist = 2;
        } else if (zroll < 90) {
            min_dist = 3; max_dist = 5;
        } else {
            min_dist = 6; max_dist = 1000000;
        }

        std::int64_t const best = bounded_best_ask_reference();
        for (int attempt = 0; attempt < 200; ++attempt) {
            std::uint64_t const id =
                resting_ids_[param_rng_.next() % resting_ids_.size()];
            auto const it = resting_orders_.find(id);
            if (it == resting_orders_.end()) continue;
            int const dist = static_cast<int>(std::abs(it->second - best));
            if (dist >= min_dist && dist <= max_dist) return id;
        }
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::uint64_t const id =
                resting_ids_[param_rng_.next() % resting_ids_.size()];
            if (resting_orders_.contains(id)) return id;
        }
        return 0;
    }

    // ================================================================
    //  Cancel cluster generation
    // ================================================================

    void enqueue_cluster_pending() {
        int const n = draw_cluster_size();
        if (n <= 1) return;
        std::int64_t const best = bounded_best_ask_reference();
        for (int i = 0; i < n && !resting_orders_.empty(); ++i) {
            for (int attempt = 0; attempt < 50; ++attempt) {
                std::uint64_t const id =
                    resting_ids_[param_rng_.next() % resting_ids_.size()];
                auto const it = resting_orders_.find(id);
                if (it == resting_orders_.end()) continue;
                if (std::abs(it->second - best) <= 3) {
                    PendingOp op;
                    op.type = PendingOp::kCancel;
                    op.target_id = id;
                    op.oid = 0;
                    pregen_queue_.push_back(op);
                    // Predictive tracking update
                    track_remove_predicted(id, it->second);
                    break;
                }
            }
        }
    }

    void enqueue_cluster_legacy() {
        int const n = draw_cluster_size();
        if (n <= 1) return;
        std::int64_t const best = bounded_best_ask_reference();
        for (int i = 0; i < n && !resting_orders_.empty(); ++i) {
            for (int attempt = 0; attempt < 50; ++attempt) {
                std::uint64_t const id =
                    resting_ids_[param_rng_.next() % resting_ids_.size()];
                auto const it = resting_orders_.find(id);
                if (it == resting_orders_.end()) continue;
                if (std::abs(it->second - best) <= 3) {
                    cluster_queue_.push_back(id);
                    break;
                }
            }
        }
    }

    int draw_cluster_size() {
        static constexpr int kSizes[] = {
            2,2,2,2,2,2,2,2,2,2,
            2,2,2,2,2,2,2,2,2,2,
            3,3,3,3,3,3,3,3,3,3,
            4,4,4,4,4,
            5,5,5,
            6,6,
            7,8,9,10,
            12,15,30,50,100,200};
        static constexpr std::size_t kSize =
            sizeof(kSizes) / sizeof(kSizes[0]);
        return kSizes[param_rng_.next() % kSize];
    }
};

}  // namespace

int main(int argc, char** argv) {
    BenchHftMacro scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
