# Phase 4: Price-Level Storage Strategy

## Overview

Phase 4 changes the matching engine's **price-level storage**. The current
high-level goal is to move beyond the baseline:

```cpp
std::map<price, IntrusiveList>
```

without losing correctness under the Phase 3 HFT benchmark suite.

The main design constraint is that `hft_macro` does not have a fixed price
range. Micro benchmarks use predictable prices around `1000`, but the macro
benchmark generates prices relative to a drifting best ask and uses modify
events that can move resting orders by `+/- 1..3` ticks. Therefore a pure
fixed-size `std::vector<OrderLevel>` is not a correct final design unless the
benchmark itself clamps prices, which would change the workload.

This phase should proceed as an incremental performance-engineering campaign:
one structural change per version, one benchmark pass per version, and a clear
decision gate before adding the next layer.

---

## Current State

The matching engine already has the important Phase 2/3 primitives:

| Component | Current role |
|---|---|
| `OrderPool` | Pre-allocated order node storage |
| Intrusive per-level queue | FIFO order priority without per-node list allocation |
| `absl::flat_hash_map<id, Order*>` | O(1) average cancel/modify index |
| `std::map<price, level>` | Ordered price-level storage and best-price discovery |
| HFT benchmark suite | Micro and macro workloads with realistic spatial locality |

The next bottleneck is the price-level container:

- `std::map` gives correct ordering and O(1) `begin()` best access.
- It pays pointer-chasing and red-black-tree maintenance costs.
- Most HFT operations hit prices near the inside quote, so a contiguous hot
  structure should be faster if correctness for drifting prices is preserved.

---

## Price-Range Findings From HFT Benchmarks

### Micro Benchmarks

All HFT micro benchmarks prefill a sell-side book through:

```cpp
PrefillHftBook(book, orders, levels, 1000, ...)
```

This creates ask levels:

```text
[1000, 1000 + levels - 1]
```

With the default benchmark matrix:

| `levels` | Prefilled ask range |
|---:|---|
| 10 | `[1000, 1009]` |
| 100 | `[1000, 1099]` |
| 1000 | `[1000, 1999]` |

Additional micro benchmark prices:

| Scenario | Runtime prices | Best drift |
|---|---|---|
| `hft_add_near` | buy `998..999` | best ask does not drift |
| `hft_add_far` | buy `950..990` | best ask does not drift |
| `hft_modify_near` | buy `998..999` | best ask can move if old best is emptied |
| `hft_cancel_hot` | existing ask hot zone | best ask can move if old best is emptied |
| `hft_cancel_cold` | existing ask cold zone | best ask usually unchanged |
| `hft_market_small` | consumes ask levels | best ask moves up |
| `hft_market_large` | consumes ask levels | best ask moves up more |
| `hft_cxl_miss` | no price change | no best drift |

Micro benchmarks can be handled by a fixed price window. They are not enough to
justify correctness for the macro benchmark.

### Macro Benchmark

`hft_macro` has no hard upper bound:

```cpp
ref = best_ask;
buy_price  = max(1, ref - offset - 1);  // offset in [1, 100]
sell_price = ref + offset;              // offset in [1, 100]
```

Modify events also move old prices without a clamp:

```cpp
new_price = old_price +/- delta;         // delta in [1, 3]
```

The macro benchmark therefore requires price-level storage that can represent
arbitrary prices. Any final design must retain a cold path for prices outside
the hot region.

---

## Design Principle

Do not jump directly from `std::map` to a complex hybrid structure.

The correct Phase 4 workflow is:

1. Establish a clean `std::map` baseline.
2. Refactor the price-book interface without changing behavior.
3. Replace the ordered container with a cache-friendlier ordered container.
4. Add telemetry to quantify actual price locality and drift.
5. Add a hot contiguous structure only after the baseline data justifies it.
6. Benchmark every version with the same matrix and record the result.

This avoids a common performance-engineering failure mode: implementing several
optimizations at once and then being unable to explain the result.

---

## Version Plan

### V0: Restore `std::map` Baseline

**Goal**: return to a buildable, testable reference implementation.

Structure:

```cpp
using AskBook = std::map<std::int64_t, PriceLevel, std::less<>>;
using BidBook = std::map<std::int64_t, PriceLevel, std::greater<>>;
```

Acceptance criteria:

- `cmake` configures successfully.
- `matching_core_tests` pass.
- `benchmark_smoke_test` passes.
- HFT canary benchmarks run with `VERSION_TAG=v0_map_baseline`.

This version is the reference for all later changes.

### V1: Introduce `SideBook`, Still Backed by `std::map`

**Goal**: isolate price-level storage behind a small API while preserving the
same behavior and the same container.

Sketch:

```cpp
template <bool IsAsk>
class SideBook {
public:
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::int64_t best_price() const;
    [[nodiscard]] PriceLevel* best_level();

    PriceLevel& get_or_create(std::int64_t price);
    PriceLevel* find(std::int64_t price);
    void erase_if_empty(std::int64_t price);

private:
    using Comparator = std::conditional_t<IsAsk, std::less<std::int64_t>,
                                          std::greater<std::int64_t>>;
    std::map<std::int64_t, PriceLevel, Comparator> levels_;
};
```

Expected benchmark result:

- Throughput should be close to V0.
- Any material regression means the abstraction introduced overhead or changed
  behavior and must be investigated before moving on.

Decision gate:

- Continue only if V1 is functionally identical and benchmark-neutral.

### V2: Replace `std::map` With `absl::btree_map`

**Goal**: test a low-risk ordered-container replacement.

`absl::btree_map` preserves the key property needed by matching:

```cpp
best price == levels_.begin()->first
```

but stores keys and values in cache-friendlier B-tree nodes than `std::map`.

Expected wins:

- Lower pointer-chasing cost.
- Better cache behavior for market sweeps and best-level churn.
- Lower memory overhead per price level.

Risks:

- Moving `PriceLevel` values must be safe. Intrusive lists are movable, but
  order nodes must not rely on stable addresses of the container's value.
- If `Order` stores `parent_level*`, verify that no container operation moves
  a live `PriceLevel` after orders have been inserted. The safer long-term
  direction is to remove `parent_level*` and locate the level by
  `(side, price)` during cancel.

Decision gate:

- If V2 improves or is neutral, keep it as the ordered cold-path baseline.
- If V2 regresses, keep V1 and move to telemetry before adding hot structures.

### V3: Add Price-Locality Telemetry

**Goal**: measure the actual distribution needed to size the hot structure.

This version should not change matching behavior. It should report:

| Metric | Purpose |
|---|---|
| min/max order price seen | Validate total price drift |
| min/max best ask and best bid | Measure inside-price drift |
| distance from best histogram | Choose hot window size |
| hot-zone hit rate for N in {32,64,128,256,512,1024} | Estimate value of hot ring |
| cold promotion/eviction count | Estimate churn |

Suggested debug-only counters:

```cpp
struct PriceTelemetry {
    std::int64_t min_price = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_price = std::numeric_limits<std::int64_t>::min();
    std::int64_t min_best_ask = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_best_ask = std::numeric_limits<std::int64_t>::min();
    std::array<std::uint64_t, 1025> distance_hist{};
};
```

Decision gate:

- Choose `HotSize` from measured hit rate, not intuition.
- Do not introduce a hot ring unless the data shows a strong near-best
  concentration under `hft_macro`.

### V4: Hot Ring + Ordered Cold Map

**Goal**: first real price-level data-structure change.

Keep correctness simple:

- Hot path: fixed-size ring or vector window near the current best.
- Cold path: ordered map (`std::map` or `absl::btree_map`) for all other prices.

Recommended invariant:

```text
ask hot window: [best_ask, best_ask + HotSize)
bid hot window: (best_bid - HotSize, best_bid]
```

This is single-sided in the depth direction. A symmetric `best +/- N` window is
less precise for an order book because asks only need increasing depth from the
best ask and bids only need decreasing depth from the best bid.

Important correctness rule:

```text
Any price must be representable either in hot or cold.
```

No order should be rejected or clamped merely because it falls outside the hot
window.

Implementation note:

- On best movement, run `recenter_hot_window(new_best)`.
- Evict non-empty hot slots outside the new window to cold.
- Promote cold levels that now fall inside the new hot window.
- Keep cold ordered so the next best can be found without scanning.

Decision gate:

- V4 must pass all correctness tests before any bitmap or cold-hash work.
- The first performance target is lower average latency in `hft_add_near`,
  `hft_cancel_hot`, and `hft_macro` without p99 spikes in market sweeps.

### V5: Add Hot Bitmap

**Goal**: optimize best-level discovery inside the hot ring.

Instead of linearly scanning hot slots after a best level is emptied, maintain
occupancy bits:

```cpp
std::array<std::uint64_t, HotSize / 64> occupied_bits_;
```

Expected wins:

- Faster market sweeps.
- Lower p95/p99 when repeated cancels or market orders empty best levels.

Decision gate:

- Keep this version only if it improves market and macro tail latency.
- If average improves but p99 regresses, inspect recentering and bit operations.

### V6: Cold Container Experiments

**Goal**: optimize the cold path only after the hot structure is correct.

Candidates:

| Design | Exact price lookup | Cold best lookup | Complexity |
|---|---:|---:|---|
| `std::map` | O(log C) | O(1) via `begin()` | low |
| `absl::btree_map` | O(log C) | O(1) via `begin()` | low-medium |
| `absl::flat_hash_map + btree_set` | O(1) avg | O(1) via set begin | medium |
| `absl::flat_hash_map + heap` | O(1) avg | amortized O(log C) | medium-high |

Do not start here. Cold-path container experiments are only useful if telemetry
and V4/V5 benchmark data show cold activity is material.

---

## Benchmark Methodology

Every version must use the same benchmark matrix.

### Build and Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLMES_BUILD_BENCHMARKS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

### HFT Micro Canary

```bash
SCENARIOS=hft_add_near,hft_add_far,hft_cancel_hot,hft_cancel_cold,hft_modify_near,hft_market_small,hft_market_large \
METRICS=latency TRIALS=3 ITERS=1000 WARMUP_ITERS=100 VERSION_TAG=vX \
bash benchmark/scripts/run_benchmarks.sh
```

### HFT Macro Canary

```bash
SCENARIOS=hft_macro METRICS=latency ORDERS=100000 LEVELS=100 \
BATCH_SIZES=100000 TRIALS=3 ITERS=1 WARMUP_ITERS=0 VERSION_TAG=vX \
bash benchmark/scripts/run_benchmarks.sh
```

### Merge Results

```bash
python3 benchmark/scripts/merge_benchmark_metrics.py
```

### Metrics to Track

| Metric | Why it matters |
|---|---|
| `ops_s` | headline throughput |
| `avg_ns` | steady-state average cost |
| `p95_ns`, `p99_ns` | tail latency and recentering spikes |
| `cache_misses_per_op` | memory behavior |
| `instructions_per_op` | algorithmic overhead |
| `cpi` | stall behavior |

PMC metrics should be collected on the remote benchmark box when available.
Local latency-only runs are acceptable for fast iteration, but not for final
phase conclusions.

---

## Correctness Requirements

The following invariants must hold for every Phase 4 version:

1. Price-time priority is preserved at each price level.
2. `cancel_order(id)` removes exactly one resting order if the id exists.
3. `modify_order(id, ...)` behaves as cancel plus add with the existing
   project semantics.
4. Market orders never rest remainder on the book.
5. Pending-cancel and duplicate-id behavior stays unchanged.
6. Arbitrary integer prices are representable.
7. The macro benchmark must not require clamping or rejecting valid prices.

Recommended long-term cleanup:

```text
Remove Order::parent_level.
```

The safer cancel path is:

```text
id_to_order_[id] -> Order*
Order has side and price
SideBook::find(price) -> PriceLevel*
PriceLevel::erase(order)
```

This avoids pointer-stability issues when price levels move between hot and
cold storage.

---

## What Not To Do Yet

Avoid these changes in the first implementation pass:

- Do not replace cold storage with only `absl::flat_hash_map`.
- Do not clamp `hft_macro` prices to fit a fixed vector.
- Do not add hot ring, bitmap, and cold hash indexing in one version.
- Do not judge a design only on micro benchmarks; `hft_macro` is the final
  workload.
- Do not keep `parent_level*` if price levels can move between containers.

---

## Recommended Next Step

Start Phase 4 with:

```text
V0: restore map baseline
V1: introduce SideBook backed by std::map
```

These two versions create a stable foundation for later container experiments.
Only after V1 is benchmark-neutral should the project move to `absl::btree_map`
or hot-ring work.

The expected final architecture, if benchmarks justify it, is:

```text
Hot ring/vector for near-best price levels
Ordered cold map for arbitrary out-of-window prices
Bitmap for hot best discovery
Optional cold hash index only if cold-path telemetry justifies it
```

This keeps the project disciplined: correctness first, measurable changes
second, complexity only when the data pays for it.
