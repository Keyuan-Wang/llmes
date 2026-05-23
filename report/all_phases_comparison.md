# Hash Table Engineering: All Phases Comparison

## Results (10 trials, orders=100,000, levels=100)

| Phase | Data Structure | Overall ops/s | vs 2b | Instr/op | CPI | Cache miss/op |
|---|---|---|---|---|---|---|
| **2b** | `std::unordered_map` | 4.73M | baseline | 868.6 | 0.88 | 7.29 |
| **2c** | Custom open-addressing + tombstones | 4.80M | **+1.4%** | 665.2 | 1.11 | 7.25 |
| **2d** | Robin Hood + backward shift | 4.60M | −2.9% | 687.0 | 1.18 | 9.16 |
| **2e** | `absl::flat_hash_map` (Swiss Table) | 4.89M | **+3.2%** | 881.7 | 0.83 | 8.58 |

*Standard deviations on overall ops/s: phase2b ±550K (11.6% CV), phase2c ±229K (4.8% CV), phase2d ±298K (6.5% CV), phase2e ±528K (10.8% CV).*

### Per-Scenario Best

| Scenario | Winner | vs 2b | Key factor |
|---|---|---|---|
| **cxl_hit** | **2b** (std) | baseline | O(1) bucket lookup beats SIMD probe and tombstone-chain walk |
| **cxl_miss** | **2e** (absl) | **+23%** | Swiss Table misses fast-reject via metadata probe |
| **dup_reject** | **2e** (absl) | **+7%** | Fast miss detection + high IPC |
| **lmt_rest** | **2c** (custom) | **+50%** | No malloc per insert; lowest instruction count |
| **lmt_cross_shallow** | **2c** (custom) | **+47%** | Instruction savings dominate (1.60M → 0.95M) |
| **lmt_cross_deep** | **2c** (custom) | **+1%** | Slight instruction advantage |
| **mkt_sweep_deep** | **2c** (custom) | **+15%** | 40% fewer instructions per op |
| **overall** | **2e** (absl) | **+3.2%** | Narrow win across the mixed workload |

## Analysis

### Phase 2c — Custom Open Addressing (+1.4%)
The biggest win in the scenarios that matter most for pure matching (lmt_rest +50%, mkt_sweep_deep +15%). Instructions drop 23% globally by eliminating `malloc`/`free` per order insertion. The tombstone problem shows in cxl_hit (−57%), but the other gains mostly compensate. The most cost-effective change: 2 header files, ~120 lines of code.

### Phase 2d — Robin Hood + Backward Shift (−2.9%)
Does not improve over phase2b. At 60% load factor, tombstone accumulation is bounded by the rehash trigger, so the backward-shift compaction overhead during erase is never recouped. CPI increases from 0.88 to 1.18 as the compaction loop touches extra cache lines.

### Phase 2e — `absl::flat_hash_map` (+3.2%)
The winner by a narrow margin. The Swiss Table's SIMD metadata probe excels on miss paths (cxl_miss +23%, dup_reject +7%) but regresses on hit paths (cxl_hit −15%) where a simple bucket lookup suffices. Overall, the mixed workload tilts just barely in its favor.

## Conclusion

| Phase | Change | Overall | Lines of code |
|---|---|---|---|
| 2b | `std::unordered_map` (baseline) | — | 0 |
| 2c | Custom open-addressing + tombstones | **+1.4%** | ~120 |
| 2d | Robin Hood + backward shift | −2.9% | ~200 |
| 2e | `absl::flat_hash_map` | **+3.2%** | 3 (plus library) |

The spread from worst to best is only 6.1 percentage points. For this workload — a matching engine dominated by 65% cancel/modify operations — the choice of hash table is not the primary bottleneck. The pool allocator (Phase 2a) and O(1) cancel index (Phase 2b) were the transformative changes. Further hash table tuning yields diminishing returns.
