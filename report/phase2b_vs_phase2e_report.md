# Phase2b vs Phase2e: absl::flat_hash_map vs std::unordered_map

## What Changed

Phase2e replaces `std::unordered_map<uint64_t, Order*>` in the `id_to_order_` cancel index with Google Abseil's `absl::flat_hash_map`. This is the production-grade Swiss Table implementation:

- **16-way SIMD group lookup** — metadata bytes scanned in parallel for fast empty/deleted skipping
- **Separate metadata array** — control bytes stored apart from key-value slots, reducing cache pollution during probe
- **Power-of-2 capacity with tombstones** — similar strategy to the Phase2c hand-rolled table, but SIMD-accelerated

All other code (pool allocator, intrusive list, matching logic) remains identical to Phase2b.

## Benchmark Results (orders=100,000, levels=100)

| Scenario | Metric | phase2b (std) | phase2e (absl) | Change |
|---|---|---|---|---|
| **lmt_rest** | Throughput | 11.5M ops/s | 17.2M ops/s | **+49.9%** |
| | Instructions/op | 842 | 554 | −34.1% |
| | CPI | 0.39 | 0.42 | +7.8% |
| **lmt_cross_shallow** | Throughput | 10.1K ops/s | 15.1K ops/s | **+50.5%** |
| | Instructions/op | 1.60M | 0.98M | −38.9% |
| | Cache misses/op | 1,684 | 7,807 | +363.5% |
| **mkt_sweep_deep** | Throughput | 137.9K ops/s | 147.0K ops/s | **+6.6%** |
| | Instructions/op | 108.6K | 66.3K | −38.9% |
| | CPI | 0.25 | 0.43 | +70.7% |
| **lmt_cross_deep** | Throughput | 242.0K ops/s | 233.0K ops/s | **−3.7%** |
| | Cache misses/op | 219 | 571 | +161.4% |
| **cxl_miss** | Throughput | 15.4M ops/s | 14.1M ops/s | **−8.2%** |
| **cxl_hit** | Throughput | 3.98M ops/s | 2.24M ops/s | **−43.8%** |
| | CPI | 3.42 | 6.85 | +100.2% |
| | Cache misses/op | 84.5 | 112.5 | +33.2% |
| **dup_reject** | Throughput | 56.8M ops/s | 38.7M ops/s | **−31.9%** |
| | CPI | 0.69 | 1.01 | +45.0% |
| **overall** | Throughput | 4.87M ops/s | 4.67M ops/s | **−4.1%** |
| | Instructions/op | 869 | 687 | −20.9% |
| | CPI | 0.90 | 1.24 | +37.5% |
| | Cache misses/op | 7.02 | 9.31 | +32.6% |

## Analysis

### Insert-Heavy Scenarios Win (lmt_rest, lmt_cross_shallow, mkt_sweep_deep)

The Swiss Table's `insert` is significantly cheaper than `std::unordered_map::emplace`:

- `std::unordered_map` allocates a heap node per insert (malloc + free on erase)
- `absl::flat_hash_map` stores all entries inline in a slot array — no per-entry allocation

This matches the same advantage the Phase2c custom flat hash table showed. In insert-heavy paths (limit orders resting, new orders placed during matching), instructions drop ~35% and throughput improves 7–50%.

The `lmt_cross_shallow` case is striking: throughput +50.5% despite cache misses increasing 4.6×. The instruction reduction (1.60M → 0.98M) dominates the cache penalty.

### Erase-Heavy Scenarios Lose (cxl_hit, dup_reject)

The `absl::flat_hash_map` shows significant regression in scenarios dominated by `find()` and `erase()`:

**cxl_hit (−43.8%):** Every operation does one `find()` and one `erase()`. CPI doubles from 3.42 to 6.85. The Swiss Table's erase is more expensive because:
- It must tombstone the slot metadata byte and may trigger probe-chain fixup
- The metadata array access pattern is less predictable than a simple bucket lookup

**dup_reject (−31.9%):** Every operation does one `find()` (miss case). Same instruction count (123), but CPI increases 45%. The `std::unordered_map` does a single bucket hash + linked-list walk; `absl::flat_hash_map` does a SIMD probe of the metadata group. For a miss in a sparsely-populated table, the SIMD probe touches more cache lines than a single bucket check.

### Overall: −4.1%

The mixed workload cancels out the gains and losses:

- Insert-heavy scenarios (lmt_rest, lmt_cross, mkt: 35% of operations) benefit from lower instruction count
- Erase-heavy scenarios (cancel, modify: 65% of operations) suffer from higher CPI

The net result is −4.1% throughput. Instructions drop 20.9%, but CPI rises 37.5%, and cache misses increase 32.6%.

## Comparison Across All Phases

| Phase | Data Structure | Overall Throughput | vs Phase2b |
|---|---|---|---|
| 2b | `std::unordered_map` | 4.87M ops/s | baseline |
| 2c | Custom open-addressing + tombstones | — | — |
| 2d | Custom Robin Hood + backward shift | — | — |
| 2e | `absl::flat_hash_map` | 4.67M ops/s | −4.1% |

## Conclusion

`absl::flat_hash_map` is not a clear win over `std::unordered_map` for this workload. The Swiss Table's SIMD-accelerated lookup excels in insert-heavy paths but regresses in erase-heavy paths.

The lesson: for an order-book matching engine dominated by cancel and modify operations (65% of the mix), `std::unordered_map` remains a competitive choice for the id-to-order index. The node-based allocation overhead is mitigated by the relatively small number of active orders (~100K), and the O(1) bucket lookup for find/erase is hard to beat in cache behavior.

This closes the Phase 2 optimization loop. The key finding across all phases:

| Phase | Change | Overall vs 2b |
|---|---|---|
| 2a | Pool allocator + intrusive list | — |
| 2b | `std::unordered_map` index | baseline |
| 2c | Custom open-addressing + tombstones | −2.9% |
| 2d | Robin Hood + backward shift | −4.5% (vs 2c) |
| 2e | `absl::flat_hash_map` | −4.1% |
