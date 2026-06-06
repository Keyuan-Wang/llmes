# Phase 7 Benchmark Results

## Environment

All runs on the same Hetzner CCX23 instance (AMD EPYC Milan, 4 vCPU, 32 GB RAM, KVM). Compiler: GCC 15.2.0, `-O3`. Scenario: `hft_macro` with `orders=100000`, `levels=100`, `batch_size=100000`, `warmup_iters=1`, `iters=1`, `seed=42`.

## Experiment 1: Phase 7 vs Phase 6b (std::map + PMR rolled back)

10 trials. master @ `bc70159` (ring + cold) vs phase6b @ `4faa7f1` (naive `std::map` SideBook).

| Metric | master | phase6b | Change |
|---|---:|---:|---:|
| avg ns/op | **24.19** | 31.85 | **−24.1%** |
| ops/s | **41.3M** | 31.4M | **+31.7%** |
| instructions/op | **177.3** | 218.1 | **−18.7%** |
| branches/op | **36.1** | 47.9 | **−24.7%** |
| branch misses/op | **1.47** | 2.16 | **−32.1%** |
| CPI | 0.495 | 0.529 | −6.4% |
| cache misses/op | 0.041 | 0.032 | +29% |

95% CIs do not overlap: master [23.89, 24.49] vs phase6b [31.71, 31.99].

Artifacts: `server_results/compare_master_vs_phase6b_20260605_181141/`

## Experiment 2: Phase 7 vs Phase 6a (std::map, no PMR, handle-based)

10 trials. master @ `00e6470` vs phase6a @ `d778e4f`.

| Metric | master | phase6a | Change |
|---|---:|---:|---:|
| avg ns/op | **24.26** | 30.30 | **−19.9%** |
| ops/s | **41.2M** | 33.0M | **+24.9%** |
| instructions/op | **177.3** | 183.6 | **−3.4%** |
| branches/op | **36.1** | 41.4 | **−12.8%** |
| branch misses/op | **1.48** | 2.17 | **−32.2%** |
| CPI | **0.508** | 0.598 | **−15.1%** |
| cache misses/op | 0.038 | 0.035 | +7.5% |

95% CIs do not overlap: master [23.86, 24.66] vs phase6a [29.99, 30.61].

Artifacts: `server_results/compare_master_vs_phase6a_20260605_182321/`

### Why two baselines

Phase 6b = Phase 6a + PMR node pool (rejected). Phase 6a is the cleaner comparison because it isolates the ring vs `std::map` delta without the PMR noise. The two experiments serve as mutual cross-validation: master's latency is consistent across both (24.19 vs 24.26 ns/op).

### Where the speedup comes from

Phase 6a already removed the hash-map cancel index (Phase 6a handle migration), so the remaining `std::map` cost was the dominant bottleneck. Comparing master against phase6a shows:

- **Instructions dropped 3.4%** (177.3 vs 183.6). The gap is modest because phase6a was already lean — ring eliminates `lower_bound` + `try_emplace` + RB-tree rebalance, replacing them with `rank` + `idx_of` + one array load.
- **CPI dropped 15.1%** (0.508 vs 0.598). This is the dominant contributor. The ring's array index hits L1 directly, while `std::map`'s pointer-chasing tree traversal causes branch mispredictions and cache stalls.
- **Branch misses dropped 32.2%** (1.48 vs 2.17). This is the direct signature of the RB-tree comparison chain disappearing from the hot path.
- **Cache misses rose 7.5%** (0.038 vs 0.035). A minor regression — the ring + cold map is two data structures where `std::map` was one. But the instruction and CPI savings far outweigh this cost.

Comparing against phase6b amplifies the gap (−24.1% latency, −18.7% instructions) because phase6b's PMR layer added extra instruction overhead on top of phase6a.

## Experiment 3: RingSize Sweep (8, 16, 32, 64)

30 trials per configuration. Single commit `fc971b9`, same machine.

| RingSize | avg ns/op | 95% CI | ops/s | instr/op | branch miss/op | cache miss/op |
|---:|---:|---|---:|---:|---:|---:|
| **16** | **24.091** | [23.998, 24.184] | 41.5M | 177.5 | 1.467 | 0.040 |
| 32 | 24.100 | [24.015, 24.186] | 41.5M | 177.4 | 1.463 | 0.039 |
| 64 | 24.245 | [24.145, 24.344] | 41.2M | 179.4 | 1.465 | 0.040 |
| 8 | 25.057 | [24.967, 25.148] | 39.9M | 179.6 | 1.556 | 0.041 |

Statistical significance (Welch t-test, n=30):

| Comparison | Δ mean | p-value | Cohen's d | Bonferroni |
|---|---:|---:|---:|---|
| 16 vs 32 | 0.009 ns (0.04%) | 0.882 | 0.04 | no |
| 16 vs 64 | 0.15 ns (0.63%) | 0.025 | 0.59 | no |
| 16 vs 8 | 0.97 ns (3.86%) | <0.001 | 3.93 | **yes** |

Artifacts: `server_results/master_ring_size_sweep_trials30_20260605_185129/`

### Finding 1: RingSize=8 is too small

~4% slower than 16/32, p<0.001, survives Bonferroni correction (6 pairwise tests, threshold p<0.0083). The HFT macro workload places ~90% of operations within ±5 ticks of best. An 8-slot window cannot cover this range — near-best operations spill into the cold `std::map` path. This shows up as +2 instructions/op (179.6 vs 177.5) and +6% branch misses (1.556 vs 1.467).

### Finding 2: RingSize=16 and 32 are statistically indistinguishable

p=0.882, Cohen's d=0.04 (negligible effect). 95% CIs overlap almost entirely. Under the current workload, 16 slots already capture virtually all near-best operations. Widening to 32 adds no measurable benefit.

### Finding 3: RingSize=64 shows a weak regression trend

0.6% slower than 16, p=0.025 at α=5% but does not survive Bonferroni correction — treat as suggestive, not conclusive. The likely cause is +2 instructions/op (179.4 vs 177.5), possibly from different compiler codegen for the larger array (loop unrolling thresholds, `flush_all_to_cold` iterating 64 slots). Cache misses are identical (0.040), so it is not a footprint issue.

### RingSize conclusion

RingSize=16 is optimal for the current workload. It matches 32 in performance while using half the footprint (384B vs 768B ring array). The `uint_from_size<16>` trait maps `MaskType` to `uint16_t`, enabling `std::rotr` to perform a natural 16-bit rotation with no masking overhead.

## Experiment 4: Production `perf record` (RingSize=16)

Window-isolated RunOp profiling on `master` @ `fd1436c` (`RingSize=16` default). Script: `benchmark/scripts/run_hft_macro_perf_record.sh`. Build: Release + `-g`, no `LLMES_PROFILE_*`. Sampling: `cycles,branch-misses` @ 8000 Hz, enabled only around the measured RunOp batch (`perf --control=fifo`, `-D -1`). Workload: `orders=100000`, `levels=100`, `batch_size=1_000_000`, `iters=40`. Samples: 20,783 cycles events, 0 lost.

Wall-clock latency for this run: avg **32.6 ns/op** over the 1M batch — not directly comparable to the 100k macro campaign numbers above.

Artifacts: `server_results/hft_macro_perf_record_ring16_20260605_180400/`

### Top-level `execute_pending` breakdown

| Operation | % RunOp cycles |
|---|---:|
| `add_limit_order` | **49.3** |
| `cancel_order` | 11.4 |
| `modify_order` | 9.8 |
| `add_market_order` | 2.9 |

`perf annotate` produced empty symbol files because `-O3` fully inlines engine calls into `execute_pending`; the breakdown below comes from `report.txt` call chains.

### `add_limit_order` — total footprint

`add_limit_order` accounts for **~57%** of all RunOp cycles, from three paths:

| Call path | % RunOp cycles |
|---|---:|
| `execute_pending` direct call | **49.26** |
| Nested inside `modify_order` | **8.25** |
| Other | ~0.5 |

The detailed tree below uses the **57.01%** aggregated symbol view. All percentages are **relative to total RunOp cycles**, not relative to `add_limit_order` itself.

### `add_limit_order` — primary breakdown

Corresponds to `OrderBook::add_limit_order`: `match_against` (cross opposite side), then `pool_.acquire` + `get_or_create` + `push_back` for the resting remainder.

| Phase | Symbol / meaning | % RunOp | % of add_limit† |
|---|---|---:|---:|
| **A. Matching (`match_against`)** | | **~14.8** | **~26%** |
| └ Consume asks (buy limit) | `operator()<CachedSideBook<true>>` | 5.97 | 10.5% |
| └ Consume bids (sell limit) | `operator()<CachedSideBook<false>>` | 8.81 | 15.5% |
| **B. Resting remainder** | | **~20.4** | **~36%** |
| └ Acquire order node | `OrderPool::acquire` | 3.09 | 5.4% |
| └ Resolve handle | `resolve` / `operator[]` | 1.16 | 2.0% |
| └ Get/create price level | `CachedSideBook::get_or_create` | 12.34 | 21.6% |
| └ Enqueue into level FIFO | `PriceLevel::push_back` | 4.95 | 8.7% |
| **C. Ring re-anchor** | `reanchor_to` | 6.46 | 11.3% |
| **D. Unattributed / inlined body** | prologue, branches, small fragments | **~14.2** | **~25%** |

†% of add_limit = item / 57.01%.

### A. `match_against` detail

**Buy limit → consume `asks_` (`CachedSideBook<true>`, 5.97%)**

| Sub-item | % RunOp |
|---|---:|
| `erase_best` (advance after level drained) | 3.23 |
| `empty` checks | 0.76 |
| `best_level` / pointer fetch | 0.71 |
| `remove` → `reset` → `cfree` (under `erase_best`) | ~1.17 |

**Sell limit → consume `bids_` (`CachedSideBook<false>`, 8.81%)**

| Sub-item | % RunOp |
|---|---:|
| `erase_best` | 4.23 |
| `empty` checks | 2.31 |
| `best_level` | 0.64 |
| `emplace_back` (`AddResult::trades`) | 0.70 |
| `remove` → `reset` → `cfree` | ~1.50 |

Matching cost is dominated by **loop emptiness checks + `erase_best` advancement + slot teardown (`cfree`)**, not hash-map cancel-index work (removed in Phase 6a).

### B. `get_or_create` detail (12.34%)

| Sub-item | % RunOp | Notes |
|---|---:|---|
| Hot-ring `materialize` | 5.76 | New hot slot → `make_unique<PriceLevel>` |
| └ `operator new` → `malloc` | 3.37 → 1.84 | Heap allocation for hot price level |
| Cold-map `cold_get_or_create` | 2.24 | Tick outside ring window |
| └ `try_emplace` → map node alloc | 1.78 → 0.76 | `std::map` node allocation |
| └ `lower_bound` | 0.54 | Cold-path lookup |

Hot-ring level creation (~5.8%) remains the largest single block inside `get_or_create`; cold `std::map` work is ~2.2%.

### C. `reanchor_to` detail (6.46%)

| Sub-item | % RunOp |
|---|---:|
| `materialize` (new hot level after re-anchor) | 2.49 |
| └ `malloc` | 1.56 |
| `flush_all_to_cold` (large price jump) | 1.71 |
| └ `remove` (clear slots) | 0.82 |
| `set_anchor` | 0.54 |

Ring maintenance on best-price jumps is a **new Phase 7 cost center** not present in the pure `std::map` baseline.

### `add_limit_order` via `modify_order` (8.25%)

The modify path re-enters `add_limit_order` after cancel semantics. Its profile is **match-heavy**, not rest-heavy:

| Sub-item | % RunOp |
|---|---:|
| `CachedSideBook<false>` match lambda | 2.24 |
| `CachedSideBook<true>` match lambda | 1.91 |
| `reanchor_to` | 1.51 |
| `get_or_create` | 0.80 |

Little `push_back` / `acquire` appears here — modify mostly re-crosses the book rather than cold-resting a new level.

### `add_limit_order` cycle tree (summary)

```
add_limit_order total ~57%
├─ match_against (crossing)           ~14.8%
│  ├─ bids-side lambda                 8.8%  (erase_best 4.2, empty 2.3)
│  └─ asks-side lambda                 6.0%  (erase_best 3.2)
├─ get_or_create (price level)        12.3%
│  ├─ hot materialize + malloc         5.8%
│  └─ cold map emplace                 2.2%
├─ reanchor_to (ring maintenance)      6.5%
│  ├─ materialize + malloc           2.5%
│  └─ flush_all_to_cold              1.7%
├─ push_back (FIFO enqueue)           5.0%
├─ acquire (order pool)               3.1%
├─ resolve                            1.2%
└─ unattributed inlined body         ~14%
```

### Comparison to Phase 6a perf record

| Hot spot | Phase 6a (prior profile) | Phase 7 Ring16 (this run) |
|---|---:|---:|
| `add_limit_order` total | ~55% | ~57% |
| `get_or_create` / map | ~28% (pure `std::map`) | ~12% (hot ring + cold map) |
| `reanchor_to` | — | **6.5%** (new) |
| Cancel-index hash | removed in 6a | N/A |

Phase 7 compresses the old ~28% `std::map` `get_or_create` block to ~12%, but **`reanchor_to` + hot-ring `materialize`** absorb much of the savings. Matching-side `erase_best` / `empty` still totals ~15% of RunOp cycles.

## Cross-Phase Summary

Headline `hft_macro` latency across all measured phases, from the unified campaign and Phase 7 results:

| Phase | avg ns/op | ops/s | Key change |
|---|---:|---:|---|
| Phase 1 | 2170 | 0.47M | `std::list`, O(N) cancel |
| Phase 2a | 2137 | 0.47M | pool-backed intrusive list |
| Phase 2b | 48.3 | 20.7M | `unordered_map` O(1) cancel index |
| Phase 2e | 39.8 | 25.2M | `absl::flat_hash_map` (Swiss Table) |
| Phase 4a | 39.3 | 25.5M | `SideBook` abstraction |
| Phase 5 | 34.4 | 29.1M | production profiling baseline |
| Phase 6a | 30.3 | 33.0M | handle-based cancel, hash map removed |
| **Phase 7** | **24.2** | **41.3M** | **hot ring buffer + cold std::map** |

Phase 1 → Phase 7 cumulative: **2170 → 24.2 ns/op, 90× throughput improvement**.

Phase 6a → Phase 7: **30.3 → 24.2 ns/op, 20% latency reduction, 25% throughput gain**.
