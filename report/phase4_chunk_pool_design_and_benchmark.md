# Phase 4: ChunkPool Design and Benchmark Report

## Overview

This report documents the Phase 4 experiment that replaces the old global
order-pool storage with a chunk-based per-price-level allocator:

```text
Slot -> Chunk -> ChunkPool -> PriceLevel -> OrderBook
```

The goal was not to change matching semantics. The goal was to test whether
making orders cache-local within each price level can improve the hot path of
the matching engine while preserving:

- pointer stability for live `Order*`
- O(1) average cancel/modify lookup through `id_to_order_`
- O(1) per-level order allocation and release
- no `new` / `delete` on the hot path
- fixed-capacity memory behavior suitable for HFT-style systems

The benchmark campaign in:

```text
benchmark/results/campaign_20260601_1319
```

compares:

- `phase4a`: the previous global pool + intrusive list baseline
- `master` commit `b0e1aaa`: ChunkPool implementation
- `kChunkSize = 16, 32, 64, 128, 256`

with 10 trials, `orders=100000`, `levels=100`.

The result is clear:

- `hft_macro` is performance-neutral.
- Micro benchmarks mostly regress.
- Macro cache miss rate is very low, so cache miss is not the bottleneck in the
  current macro workload.
- The current ChunkPool design adds enough instruction and pointer-management
  overhead to outweigh the intended locality benefit in micro hot paths.

---

## Why ChunkPool Was Introduced

The previous design used one large order storage area for the whole book. Each
price level stored an intrusive FIFO list of `Order` nodes, but the actual order
objects came from one shared pool.

That design has important strengths:

- one simple global allocator
- stable `Order*` when the backing pool does not resize
- cheap O(1) cancel path:

```text
order_id -> Order* -> parent PriceLevel -> unlink from FIFO
```

However, it has a locality problem. Orders belonging to the same price level can
be spread across the global pool. In a matching engine, the hot area is usually
near the best price. If one hot price level contains many orders, then matching
or repeated cancel/modify operations on that level may jump across memory.

The ChunkPool experiment tests the opposite layout:

```text
one PriceLevel owns one or more cache-local chunks
orders in the same chunk are physically contiguous
empty chunks are returned to a global pool
```

The intended benefit is:

- matching through a dense price level should touch nearby memory
- allocating another order at a hot price should reuse that level's current
  chunk
- cancellation should still remain O(1), because `Order*` and
  `Order::parent_level` are preserved

This is a reasonable hypothesis for an HFT matching engine. The benchmark result
shows that the hypothesis does not currently hold for this implementation and
workload mix.

---

## Data Structure Design

### Slot

`Slot` is the smallest allocation unit inside a chunk:

```cpp
struct Slot {
    Order order;
    Slot* next_free = nullptr;
};
```

The important separation is:

- `Order::prev` / `Order::next` are used by the live FIFO queue of a
  `PriceLevel`.
- `Slot::next_free` is used only when the slot is free.

This avoids overloading `Order::next` for two meanings. A live order belongs to
the matching engine's FIFO queue. A free slot belongs to the allocator's free
list.

### Chunk

Each `Chunk` owns a fixed-size array of slots:

```cpp
std::array<Slot, kChunkSize> slots_;
```

and allocator metadata:

```cpp
Slot* free_head_;
std::uint16_t free_count_;
Chunk* prev_available_;
Chunk* next_available_;
Chunk* next_free_pool_;
```

The chunk provides:

| Function | Purpose |
|---|---|
| `allocate_order()` | pop one free slot and return `Order*` |
| `release_order()` | clear an order and push its slot back to the chunk-local free list |
| `full()` | O(1) test: no free slots |
| `empty()` | O(1) test: all slots are free |
| `link_available()` | link this chunk into one `PriceLevel`'s available-chunk list |
| `unlink_available()` | remove this chunk from that available-chunk list |

There are two different chunk lists:

```text
PriceLevel available list:
    chunks owned by one PriceLevel and still containing free slots

ChunkPool free list:
    completely empty chunks not owned by any PriceLevel
```

These lists must stay separate. A chunk with live orders belongs to a
`PriceLevel`. A completely empty chunk belongs to `ChunkPool`.

### ChunkPool

`ChunkPool` owns one fixed contiguous array:

```cpp
std::unique_ptr<Chunk[]> chunks_;
```

This gives two properties:

1. `Chunk*` addresses are stable because the array never resizes.
2. `Order* -> Chunk*` can be recovered with address arithmetic.

The key operation is:

```cpp
Chunk* ChunkPool::chunk_from_order(Order* order) noexcept
```

Because every `Order` lives inside some `Slot`, and every `Slot` lives inside
some `Chunk`, the pool can compute the containing chunk from the byte offset
between the order address and the beginning of the `Chunk[]` allocation.

This avoids storing a back-pointer from every order to its chunk.

### PriceLevel

`PriceLevel` does not own raw memory. It borrows chunks from `ChunkPool`.

It stores:

```cpp
ChunkPool* chunk_pool_;
Chunk* available_head_;
Order* head_;
Order* tail_;
std::size_t size_;
```

The live orders are still one FIFO queue:

```text
head_ -> order -> order -> ... -> tail_
```

The allocator side is separate:

```text
available_head_ -> chunk with free slot -> chunk with free slot -> ...
```

Full chunks are not in the available list. Empty chunks are returned to
`ChunkPool` immediately.

---

## Hot-Path Mechanics

### Add / Resting Limit Order

When a limit order has remaining quantity and must rest:

1. `OrderBook` finds or creates the `PriceLevel`.
2. `PriceLevel::allocate()` checks `available_head_`.
3. If no owned chunk has free slots, it gets an empty chunk from `ChunkPool`.
4. `Chunk::allocate_order()` pops one slot from the chunk-local free list.
5. The returned `Order*` is filled and pushed into the level FIFO queue.

Complexity:

```text
O(1) allocation inside the level
O(1) chunk acquisition when a new chunk is needed
```

### Cancel / Modify Remove Path

Cancel remains:

```text
id_to_order_[order_id] -> Order*
Order::parent_level -> PriceLevel
PriceLevel::remove(order)
```

Inside `PriceLevel::remove()`:

1. unlink the order from the FIFO queue
2. compute the owning `Chunk*` from the `Order*`
3. push the slot back to the chunk-local free list
4. if the chunk moved from full to partially free, link it into the available
   list
5. if the chunk became empty, remove it from the available list and return it
   to `ChunkPool`

Complexity:

```text
O(1) id lookup, average case
O(1) FIFO unlink
O(1) Order* -> Chunk* address calculation
O(1) chunk state update
```

### Capacity Model

Chunk capacity is not just:

```text
ceil(order_capacity / kChunkSize)
```

because chunks are owned by one price level at a time. Fragmentation matters.
If there are many active price levels and each has only one order, each level
still needs its own chunk.

The fragmentation-safe constructor uses:

```cpp
active_levels + (order_capacity - active_levels) / kChunkSize
```

where:

```cpp
active_levels = min(order_capacity, max_active_levels)
```

Interpretation:

- every active level may need one first chunk
- after that, each additional full block of `kChunkSize` orders needs one more
  chunk

This is the right capacity model only if `max_active_levels` is a real upper
bound for the workload.

---

## Benchmark Summary

### Scenario-Level Throughput

Source:

```text
benchmark/results/campaign_20260601_1319/overall_summary.csv
```

| Scenario | Phase4a ops/s | Best chunk | Best chunk ops/s | Best chunk vs Phase4a | Interpretation |
|---|---:|---|---:|---:|---|
| `hft_add_far` | 13.12M | `chunk16` | 11.93M | -9.1% | regression |
| `hft_add_near` | 23.86M | `chunk32` | 21.44M | -10.2% | regression |
| `hft_cancel_cold` | 12.58M | `chunk32` | 10.46M | -16.8% | large regression |
| `hft_cancel_hot` | 13.17M | `chunk256` | 10.71M | -18.7% | large regression |
| `hft_cxl_miss` | 69.47M | `chunk16` | 70.17M | +1.0% | neutral/slight win |
| `hft_macro` | 15.96M | `chunk64` | 16.08M | +0.7% | statistically neutral |
| `hft_market_large` | 66.54M | `chunk64` | 62.32M | -6.3% | regression |
| `hft_market_small` | 75.90M | `chunk64` | 65.82M | -13.3% | regression |
| `hft_modify_near` | 7.62M | `chunk16` | 6.43M | -15.6% | large regression |

### Geomean Throughput

| Version | All scenarios | Micro only |
|---|---:|---:|
| `master_chunk16` | -11.2% | -12.4% |
| `master_chunk32` | -11.7% | -12.7% |
| `master_chunk64` | -11.7% | -13.2% |
| `master_chunk128` | -13.2% | -14.4% |
| `master_chunk256` | -13.0% | -14.4% |

`kChunkSize=16` is the best of the current chunk variants, but it is still
materially slower than `phase4a` overall.

---

## Macro Benchmark Detail

For `hft_macro`, the chunk versions are effectively tied with `phase4a`.

| Version | ops/s | avg ns/op | cycles/op | instr/op | cache miss/op | MPKI | CPI |
|---|---:|---:|---:|---:|---:|---:|---:|
| `phase4a` | 15.96M | 62.727 | 228.7 | 482.0 | 0.905 | 1.878 | 0.474 |
| `chunk16` | 15.81M | 63.331 | 231.2 | 505.7 | 0.875 | 1.730 | 0.457 |
| `chunk32` | 15.56M | 64.308 | 231.2 | 505.8 | 0.884 | 1.747 | 0.457 |
| `chunk64` | 16.08M | 62.267 | 230.0 | 505.4 | 0.940 | 1.861 | 0.455 |
| `chunk128` | 15.43M | 64.882 | 231.9 | 506.1 | 0.938 | 1.854 | 0.458 |
| `chunk256` | 15.68M | 63.861 | 228.1 | 504.2 | 0.968 | 1.920 | 0.452 |

The key observation is MPKI:

```text
cache misses per 1000 instructions ~= 1.7 to 1.9
```

That is very low. For this macro workload, cache miss rate is not the bottleneck.
The `chunk64` result is `+0.7%` over `phase4a`, but the confidence intervals
overlap, so this should be treated as neutral rather than a real win.

---

## Why Macro Performance Does Not Improve

### 1. Macro Is Not Cache-Miss Bound

The original ChunkPool hypothesis was:

```text
orders in one PriceLevel become more local
therefore fewer cache misses
therefore better throughput
```

But the macro data shows that the workload already has a very low miss rate:

```text
phase4a MPKI   = 1.878
chunk16 MPKI   = 1.730
chunk64 MPKI   = 1.861
chunk256 MPKI  = 1.920
```

Even where ChunkPool slightly reduces MPKI, the improvement is too small to
matter. The bottleneck is not waiting on memory.

### 2. The Macro Hot Path Is Broader Than Per-Level Order Storage

`hft_macro` exercises:

- `absl::flat_hash_map` lookup/erase/insert for `id_to_order_`
- `std::map` price-level lookup and best-price access
- crossing checks
- FIFO unlinking
- trade/result bookkeeping
- branch-heavy event mix dispatch

ChunkPool only optimizes one part of that path: storage layout of orders inside
one price level. It does not remove:

- the hash-table lookup
- the ordered-map price-level access
- the parent-level pointer hop
- the control-flow cost of add/cancel/modify/market dispatch

When the broader macro path is already running at roughly 230 cycles/op, the
allocator layout alone is not enough to move the total.

### 3. The Old Pool Was Already Good Enough for Macro

The old global pool is not perfectly level-local, but it is still a fixed
pre-allocated pool with stable pointers. It does not allocate on the hot path.
It also has very little allocator metadata to update per operation.

For macro, this matters more than theoretical per-level locality:

```text
simple global pool:
    less allocator bookkeeping

chunk pool:
    better conceptual locality
    more allocator state transitions
```

The benchmark shows that the extra state transitions do not buy enough locality
to improve macro throughput.

---

## Why Micro Benchmarks Regress

The micro benchmarks isolate individual hot paths. They expose the cost that
macro can hide through mixed workload effects.

### Add Regresses

`hft_add_near` and `hft_add_far` both regress:

```text
hft_add_near: best chunk = -10.2%
hft_add_far:  best chunk = -9.1%
```

The old design's add path was essentially:

```text
get level -> get order node from global pool -> push_back
```

The ChunkPool path adds:

```text
check available_head_
maybe acquire empty chunk
pop slot from chunk free list
check full()
maybe link/unlink available chunk
restore parent_level after aggregate assignment
```

These operations are O(1), but O(1) is not free. In a benchmark where each
operation is only tens of nanoseconds, a few extra loads, stores, branches, and
pointer updates are material.

### Cancel Regresses More

`hft_cancel_hot` and `hft_cancel_cold` regress heavily:

```text
hft_cancel_hot:  best chunk = -18.7%
hft_cancel_cold: best chunk = -16.8%
```

Cancel does not benefit much from contiguous per-level order storage. A cancel
starts from:

```text
id_to_order_ -> exact Order*
```

It does not scan nearby orders. It only needs to unlink one known order.

ChunkPool adds work after the unlink:

```text
Order* -> Chunk* address calculation
Slot recovery inside Chunk
free-list push
full/empty state checks
available-list link/unlink
possible return to ChunkPool
```

That allocator work is paid on every successful cancel. The locality benefit is
mostly irrelevant because the operation already jumps directly to the target
order.

### Modify Regresses Because It Is Cancel + Add

`hft_modify_near` regresses by `-15.6%` even with the best chunk size.

That is expected because modify is structurally:

```text
remove old order
add replacement order
```

It pays both sides of the ChunkPool overhead.

### Larger Chunks Are Worse

The throughput geomean worsens as chunk size grows:

```text
chunk16   best overall chunk, still -11.2%
chunk128  -13.2%
chunk256  -13.0%
```

The likely reasons are:

- larger chunks increase the memory footprint owned by each active level
- sparse levels waste more cache/TLB footprint through internal fragmentation
- chunk metadata and the touched slot may sit far apart in memory for large
  chunks
- larger chunks delay chunk release and keep more cold storage attached to a
  price level

This is why `chunk16` is the least bad configuration. Small chunks reduce
fragmentation and reduce the amount of memory tied to each active price level.

---

## Hardware Counter Interpretation

Across all comparable micro/macro configurations, the chunk versions show:

| Version | cycles/op | instructions/op | cache misses/op | CPI |
|---|---:|---:|---:|---:|
| `chunk16` | +9.9% | +8.4% | +32.4% | +1.4% |
| `chunk32` | +10.0% | +8.2% | +38.7% | +1.7% |
| `chunk64` | +10.0% | +8.1% | +55.5% | +1.8% |
| `chunk128` | +12.0% | +8.1% | +78.1% | +3.7% |
| `chunk256` | +15.0% | +8.0% | +90.6% | +6.5% |

The important point is not just cache misses. The chunk versions also execute
about 8% more instructions per operation. That means the regression is partly a
front-end / instruction-count problem, not purely a memory problem.

For `hft_macro`, the absolute cache miss rate is low enough that the extra
misses are not decisive. For micro benchmarks, the hot path is small enough
that the extra bookkeeping itself becomes visible.

---

## Design Assessment

The ChunkPool design is correct in spirit for a fixed-capacity low-latency
engine:

- no hot-path dynamic allocation
- stable `Order*`
- stable `Chunk*`
- O(1) allocation and release
- O(1) cancel still preserved
- per-level chunk ownership is cleanly separated from global chunk ownership

However, the current implementation does not prove a performance win.

The design improves a locality property that is not currently the dominant
bottleneck in `hft_macro`, while adding overhead to every add/cancel/modify
operation. That overhead is visible in micro benchmarks and mostly invisible in
macro only because macro is a mixed workload with low cache-miss pressure.

In other words:

```text
ChunkPool improves theoretical per-level locality.
The benchmarked engine is not currently limited by that locality.
The extra allocator machinery costs more than the locality is worth.
```

---

## Recommendation

Do not treat the current ChunkPool implementation as a performance improvement
over `phase4a`.

The current result should be recorded as an experimental Phase 4 branch:

- correctness path works
- benchmark process works
- `kChunkSize` sweep works
- `hft_macro` is neutral
- micro hot paths regress

If ChunkPool is revisited, the next experiments should be narrower:

1. Use `kChunkSize=16` as the only serious candidate.
2. Profile `PriceLevel::allocate()` and `PriceLevel::remove()` directly.
3. Measure L1D-specific counters, not only generic cache misses:

```text
L1-dcache-loads
L1-dcache-load-misses
LLC-loads
LLC-load-misses
dTLB-loads
dTLB-load-misses
```

4. Consider reducing allocator state transitions before trying larger chunks.
5. Consider changing chunk layout so frequently touched metadata is not placed
   after a large `slots_` array.

For the main optimization roadmap, the evidence suggests that effort should
move back to reducing hot-path instruction count and price-level lookup cost,
rather than assuming per-level order storage locality is the primary bottleneck.

