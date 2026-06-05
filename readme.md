# llmes — Low-Latency Matching & Execution Simulator

A C++20 order-matching engine evolved incrementally from a correctness-first baseline through data-structure optimization, handle-based identity, hot price-level caching, and HFT workload profiling.

---

## Current Status

| Area | Status |
|------|--------|
| Matching core | Phase 7 complete |
| Price-level storage | `CachedSideBook`: 16-slot hot ring buffer + cold `std::map` |
| Order storage | Pool-backed intrusive `PriceLevel` queues |
| Order identity | Engine-issued `OrderHandle`; no id hash table on the matching hot path |
| Order types | Limit / Market / Cancel / Modify |
| Benchmark suite | 6 legacy + 8 HFT scenarios |
| Primary benchmark | `hft_macro` Zero-Intelligence model with realistic order flow |
| Current headline | **24.2 ns/op**, **41.3M ops/s** on cloud `hft_macro` |
| Market Data / Execution / Risk | Not started |

---

## Historical Hash Table Engineering Journey

The cancel path is the dominant operation in realistic order-book workloads. Earlier phases optimized arbitrary external-order-id lookup inside the matching core:

| Phase | ID Index | Macro ops/s | vs 2b | Key Limitation |
|---|---|---|---|---|
| **2b** | `std::unordered_map` | 11.0M | baseline | Node-based: pointer chase, cache-unfriendly |
| 2c | Custom open-addressing + tombstones | 7.8M | −29% | Tombstone buildup degrades lookup under cancel-heavy load |
| 2d | Robin Hood + backward-shift deletion | 7.8M | −29% | Probe chains regress under HFT spatial concentration |
| **2e** | `absl::flat_hash_map` (Swiss Table) | **11.9M** | **+8%** | — |

**Phase 2e was the in-core hash-table winner** — the Swiss-table `absl::flat_hash_map` outperformed hand-rolled open-addressing by 52% and the baseline `std::unordered_map` by 8% under realistic HFT order flow.

Phase 6a later moved identity resolution out of the matching hot path entirely. The gateway owns external `client_order_id` validation and id-to-handle mapping; the matching core receives `OrderHandle` values and resolves them by direct pool index.

---

## Current Engine: Phase 7

Phase 7 targets the remaining post-handle bottleneck: price-level lookup on resting adds. The previous `std::map<price, PriceLevel>` `get_or_create()` path was branchy and pointer-heavy. The current design keeps arbitrary-price correctness by splitting each side book into:

```text
CachedSideBook<IsAsk>
├── RingBuffer<IsAsk> hot_   # 16 near-best ticks, O(1) index arithmetic
└── std::map<price, unique_ptr<PriceLevel>> cold_
```

Key properties:

- Hot-path near-best level lookup is `rank(price)` + `idx_of(rank)` + one slot price check.
- `live_mask_` uses a compile-time-selected unsigned type (`uint16_t` for `RingSize=16`) so `std::rotr` and `std::countr_zero` find the next live level cheaply.
- `PriceLevel` ownership moves between ring slots and the cold map via `unique_ptr`; the pointee address is stable, so `Order::parent_level` remains valid.
- The implementation keeps a strict hot-window invariant: cold prices are outside the current hot window. Lazy resident-cache variants were not adopted because `erase_best()` / matching is a low-frequency path in the current macro workload, while the strict invariant keeps `get_or_create()` simple.

Cloud `hft_macro` result at `orders=100000`, `levels=100`, `batch_size=100000`, 10 trials:

| Version | avg ns/op | ops/s | Change |
|---|---:|---:|---:|
| Phase 6a (`std::map`, handle-based) | 30.30 | 33.0M | baseline |
| **Phase 7 (hot ring + cold map)** | **24.26** | **41.2M** | **−19.9% latency** |

RingSize sweep (30 trials) found `RingSize=16` and `32` statistically equivalent, `8` too small, and `64` slightly worse. The project keeps `RingSize=16`.

---

## HFT Benchmark Suite (Phase 3)

Seven micro benchmarks isolate individual data-structure paths under HFT-realistic access patterns. The macro benchmark (Zero-Intelligence model) measures sustained throughput under a continuous mixed stream:

| Scenario | What it stresses | HFT share |
|---|---|---|
| `hft_add_near` | Insert at best ±1 tick (hot path) | ~40% |
| `hft_add_far` | Cold-path insert at deep levels | ~3% |
| `hft_cancel_hot` | Erase from dense near-best level | ~45% |
| `hft_cancel_cold` | Erase from sparse deep level | ~3% |
| `hft_modify_near` | Erase + insert at hot price | ~5% |
| `hft_market_small` | Bulk erase, 1-2 levels | ~1.7% |
| `hft_market_large` | Bulk erase, 5+ levels | ~0.3% |
| `hft_macro` | ZI model (all of the above, mixed) | definitive metric |

### Key Design Features

- **Realistic depth profile**: `PrefillHftBook` distributes orders with exponential decay from the best price (20% at tick 0, 18% at tick 1, ...)
- **Spatial locality**: 90% of operations within ±5 ticks of the best price
- **Cancel clusters**: Power-law burst sizes with temporal autocorrelation
- **Normalized latency**: Market-order latency is divided by actual match count (`filled_quantity`) via `op_normalizer()` for fair comparison

Full design rationale: `report/phase3_hft_benchmark_design.md`

---

## Build & Test

**Requirements:** CMake >= 3.20, C++20 (GCC 13+ or Clang 16+ recommended), Linux (for PMC benchmarks)

```bash
cmake -S . -B build -DLLMES_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### Benchmark Pipeline

```bash
# 1. Full benchmark matrix (latency + PMC, all scenarios × trials)
bash benchmark/scripts/run_benchmarks.sh

# 2. Merge trials, compute mean/std/CV/95% CI per config
python3 benchmark/scripts/merge_benchmark_metrics.py

# 3. Parametric plots (metric vs. orders per scenario)
python3 benchmark/scripts/plot_benchmark.py

# 4. Version-comparison plots + bar charts + %-change heatmaps
python3 benchmark/scripts/plot_version_comparison.py
```

Override parameters via environment variables:

```bash
SCENARIOS=hft_add_near,hft_cancel_hot METRICS=latency,pmc \
VERSION_TAG=v2e COMMIT_SHA=<sha> TRIALS=5 \
  bash benchmark/scripts/run_benchmarks.sh
```

---

## Architecture

The benchmark suite uses the **Strategy** pattern: each scenario implements `IBenchScenario`, and the shared `benchmark_runner` handles measurement, CSV output, and command-line parsing.

```
                   benchmark_runner.cpp
                   (ParseArgs → warmup → measure → output)
                           │
                    IBenchScenario (virtual)
               ┌───────────┼───────────────┐
          bench_lmt_rest  bench_cxl_…  bench_hft_…
               │                          │
               └──────────┬───────────────┘
                          ▼
                   bench_common.hpp
              PrefillSellBook / PrefillHftBook
```

### Measurement Modes

- **Latency** (`--metric latency`): `avg / p50 / p95 / p99` nanoseconds per op and `ops/s`, normalized per batch.
- **PMC** (`--metric pmc`): In-process hardware counters via `perf_event_open` (cycles, instructions, branches, misses, cache misses). Derived: CPI, branch miss rate.

---

## Key Results

### Phase 1 → Phase 2a (Pool Allocator)

Pool-based `PriceLevel` replaces `std::list`: 22–38% fewer instructions per op. Cancel-heavy scenarios see 2× throughput, memory latency drops dramatically (CPI −59%, cache misses −87%).

### Phase 2a → Phase 2b (O(1) Cancel Index)

`std::unordered_map<id, Order*>` eliminates O(N) book scan: cancel throughput improves **300–1800×**. Cross/match scenarios regress 40–80% due to hash-map maintenance overhead, but in a realistic mixed workload the net effect is **894× overall throughput**.

### Phase 2b → 2e (Hash Table Engineering)

Under the HFT macro benchmark (48% cancel / 45% add), custom open-addressing (2c/2d) regresses 29% — tombstones and probe chains hurt cancel-heavy access. `absl::flat_hash_map` (2e) leads at 11.9M ops/s: 52% ahead of 2c/2d and 8% ahead of 2b.

### Phase 6a (Gateway-Owned Handles)

The matching core stopped resolving arbitrary external ids. Cancels and modifies now take `OrderHandle`, resolving directly into the order pool. This removed the cancel-index hash table from the measured hot path and improved `hft_macro` from 34.4 ns/op to about 30.3 ns/op on the comparable cloud run.

### Phase 7 (Hot Ring + Cold Map)

The remaining hot `std::map` price-level lookup was replaced with a near-best ring cache plus cold ordered map. On `hft_macro`, Phase 7 reaches about 24.2 ns/op / 41.3M ops/s: a further 20% latency reduction over Phase 6a and roughly 90× throughput improvement over the Phase 1 baseline.

Full reports:
- `report/phase1_vs_phase2_report.md` — Phase 1 → 2a → 2b comparison
- `report/phase2b_to_phase_2e_comparison.md` — Hash table engineering (2b–2e)
- `report/phase3_hft_benchmark_design.md` — HFT benchmark design
- `report/phase6_engine_handle_refactor_plan.md` — gateway-owned identity and engine handles
- `report/phase7_hot_ring_cold_map_design.md` — hot ring + cold map design
- `report/phase7_benchmark_results.md` — Phase 7 benchmark results and RingSize sweep

---

## Project Layout

```
llmes/
├── CMakeLists.txt
├── readme.md
├── core/matching_core/
│   ├── include/matching/
│   │   ├── order_book.hpp          # OrderBook class
│   │   ├── cached_order_book.hpp   # hot ring + cold map side book
│   │   ├── ring_buffer.hpp         # fixed-size near-best price ring
│   │   ├── price_level.hpp         # Intrusive doubly-linked list per price
│   │   ├── order_pool.hpp          # Pre-allocated order pool
│   │   └── types.hpp               # Order, Trade, AddResult, ErrorCode
│   ├── src/
│   │   ├── order_book.cpp
│   │   └── order_pool.cpp
│   └── tests/
│       └── order_book_test.cpp
├── benchmark/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── benchmark_runner.hpp    # IBenchScenario interface
│   │   ├── benchmark_runner.cpp    # measurement runner
│   │   ├── bench_common.hpp        # PrefillSellBook, PrefillHftBook, utilities
│   │   ├── legacy/                 # 6 legacy benchmarks
│   │   └── hft/                    # 8 HFT benchmarks (micro + macro)
│   ├── scripts/
│   │   ├── run_benchmarks.sh
│   │   ├── merge_benchmark_metrics.py
│   │   ├── plot_benchmark.py
│   │   └── plot_version_comparison.py
│   └── results/                    # generated CSV + plots
├── report/                         # design docs + analysis reports
└── server_results/                 # remote benchmark run artifacts
```

---

## Design Notes

- **Correctness first**: Phase 1 used `std::map` + `std::list` for a straightforward, verifiable implementation.
- **Phase 2a**: Pool allocator eliminates `malloc`/`free` from the hot path — instruction count drops 22–38%.
- **Phase 2b**: O(1) cancel index transforms the engine for cancel-dominated workloads. The trade-off (hash-map overhead on every match) is acceptable given 97% cancellation in real markets.
- **Phase 2e**: `absl::flat_hash_map`'s Swiss-table design handles HFT spatial locality better than both `std::unordered_map` and hand-rolled open-addressing.
- **Phase 3**: HFT benchmarks replace the original ad-hoc workload mix with empirically grounded order flow: exponential depth decay, spatial concentration at the best price, cancel clusters, and a Zero-Intelligence macro model.
- **Phase 6a**: Gateway-owned identity moves external id validation and id-to-handle mapping out of the matching core; cancel/modify use pool-index handles.
- **Phase 7**: Near-best prices use a 16-slot ring buffer; arbitrary out-of-window prices remain in a cold `std::map`.

---

## Time Complexity

| Function | Average | Notes |
|---|---|---|
| `cancel_order()` | O(1) | Resolve `OrderHandle` by pool index + intrusive-list erase |
| `add_limit_order()` | O(K + 1 hot / log P cold) | Match K makers; near-best rest uses ring index, out-of-window rest uses cold map |
| `add_market_order()` | O(K + L) | Match K makers and drain L price levels |

N = total resting orders, P = cold price levels, K = makers matched, L = price levels drained.

---

## Roadmap

1. **Phase 1** ✓ — Functional core + tests + benchmark harness
2. **Phase 2** ✓ — Intrusive queue + O(1) cancel index + hash table engineering
3. **Phase 3** ✓ — HFT micro + macro benchmarks, realistic workload modeling
4. **Phase 4** ✓ — Price-level storage strategy and ChunkPool experiment
5. **Phase 5** ✓ — Production profiling with window-isolated `perf record`
6. **Phase 6** ✓ — Gateway-owned identity and handle-based matching core
7. **Phase 7** ✓ — Hot ring buffer + cold map price-level storage
8. Phase 8 — Re-profile Phase 7 and choose the next bottleneck
9. Market data, execution, risk, tslib, lfutils
