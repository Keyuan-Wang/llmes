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
 * are pre-generated in Setup() -- which is outside the timed window. Pending
 * operations store stable business order IDs; the OrderBook resolves those IDs
 * through its own live-order index.
 *
 * Reference: Gode & Sunder (1993), "Allocative Efficiency of Markets with
 * Zero-Intelligence Traders." JPE 101(1), 119-137.
 */

#include "benchmark_runner.hpp"
#include "hft_macro_workload.hpp"
#include "match_result_buffer.hpp"

#include "spsc_ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace {

enum class ResultMode {
	Book,
	OverallVector,
	OverallSpsc,
};

[[nodiscard]] ResultMode ParseResultMode(int argc, char** argv) {
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg != "--result-mode") continue;
		if (i + 1 >= argc) {
			std::cerr << "missing value for --result-mode\n";
			std::exit(2);
		}
		const std::string mode = argv[++i];
		if (mode == "book" || mode == "book_latency") {
			return ResultMode::Book;
		}
		if (mode == "overall_vector" || mode == "vector") {
			return ResultMode::OverallVector;
		}
		if (mode == "overall_spsc" || mode == "spsc") {
			return ResultMode::OverallSpsc;
		}
		std::cerr << "unknown --result-mode: " << mode << "\n";
		std::exit(2);
	}
	return ResultMode::Book;
}

template <typename Sink, bool CollectResults>
class BenchHftMacro final : public benchmark_runner::IBenchScenario {
public:
	explicit BenchHftMacro(const char* name) : name_(name) {}

	const char* Name() const override { return name_; }

	[[nodiscard]] std::uint64_t max_batch_size() const override {
		return 1'000'000;
	}

	void Setup(const benchmark_runner::Args& args,
						 std::uint64_t iter_idx) override {
		if constexpr (CollectResults) {
			results_.reserve(args.batch_size, args.batch_size + 550'000);
			results_.clear();
		}
		workload_.Setup(args, iter_idx);
	}

	bool RunOp(const benchmark_runner::Args& args, std::uint64_t iter_idx,
						 std::uint64_t batch_idx, std::uint64_t& ok) override {
		(void)args;
		(void)iter_idx;
		if constexpr (CollectResults) {
			return workload_.ExecuteAndCollect(
					static_cast<std::size_t>(batch_idx), ok, results_);
		}
		return workload_.Execute(static_cast<std::size_t>(batch_idx), ok);
	}

	void Teardown() override {
		workload_.Teardown();
	}

private:
	const char* name_;
	benchmark_runner::hft::HftMacroWorkload<false, Sink> workload_;
	benchmark_runner::MatchResultBuffer results_;
};

template <typename Sink>
class BenchHftMacroAsyncSpsc final : public benchmark_runner::IBenchScenario {
public:
	explicit BenchHftMacroAsyncSpsc(const char* name) : name_(name) {}

	const char* Name() const override { return name_; }

	[[nodiscard]] std::uint64_t max_batch_size() const override {
		return 1'000'000;
	}

	void Setup(const benchmark_runner::Args& args,
						 std::uint64_t iter_idx) override {
		results_.reset(args.batch_size, args.batch_size + 550'000);
		stop_.store(false, std::memory_order_relaxed);
		workload_.Setup(args, iter_idx);
		consumer_ = std::thread([this] { ConsumerLoop(); });
	}

	bool RunOp(const benchmark_runner::Args& args, std::uint64_t iter_idx,
						 std::uint64_t batch_idx, std::uint64_t& ok) override {
		(void)args;
		(void)iter_idx;
		return workload_.ExecuteAndCollectAsync(
				static_cast<std::size_t>(batch_idx), ok, results_);
	}

	void Teardown() override {
		stop_.store(true, std::memory_order_release);
		if (consumer_.joinable()) {
			consumer_.join();
		}
		DrainOnce();
		workload_.Teardown();
	}

private:
	[[gnu::always_inline]] inline bool DrainOnce() {
		bool drained = false;
		benchmark_runner::hft::AsyncTradeEvent event{};
		while (workload_.PopAsyncTrade(event)) {
			results_.append_trade(event.record_index, event.trade);
			drained = true;
		}
		return drained;
	}

	void ConsumerLoop() {
		while (!stop_.load(std::memory_order_acquire)) {
			if (!DrainOnce()) {
#if defined(__x86_64__) || defined(_M_X64)
				_mm_pause();
#else
				std::this_thread::yield();
#endif
			}
		}
	}

	const char* name_;
	benchmark_runner::hft::HftMacroWorkload<false, Sink> workload_;
	benchmark_runner::MatchResultBuffer results_;
	std::atomic<bool> stop_{false};
	std::thread consumer_;
};

}  // namespace

int main(int argc, char** argv) {
	using Trade = llmes::matching_core::Trade;
	using AsyncEvent = benchmark_runner::hft::AsyncTradeEvent;
	using SpscQueue = llmes::spsc::SpscRingBufferAtomicV4<AsyncEvent, 1ULL << 20>;
	using SpscSink = benchmark_runner::hft::AsyncSpscTradeSink<SpscQueue>;
	(void)sizeof(Trade);

	switch (ParseResultMode(argc, argv)) {
	case ResultMode::Book: {
		auto scen = std::make_unique<
				BenchHftMacro<llmes::matching_core::NullTradeSink, false>>(
				"hft_macro_book_latency");
		return benchmark_runner::RunScenario(*scen, argc, argv);
	}
	case ResultMode::OverallVector: {
		auto scen = std::make_unique<
				BenchHftMacro<llmes::matching_core::VectorTradeSink, true>>(
				"hft_macro_overall_vector");
		return benchmark_runner::RunScenario(*scen, argc, argv);
	}
	case ResultMode::OverallSpsc: {
		auto scen = std::make_unique<BenchHftMacroAsyncSpsc<SpscSink>>(
				"hft_macro_spsc_async");
		return benchmark_runner::RunScenario(*scen, argc, argv);
	}
	}
	return 2;
}
