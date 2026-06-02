#!/usr/bin/env bash
#
# run_hft_macro_perf_record.sh
#
# Instruction-level profiling of the hft_macro engine hot path via
# `perf record` + `perf annotate`, with the recording window restricted to
# the measured RunOp batch ONLY.
#
# Why this is not a plain `perf record ./bench_hft_macro`:
#   The benchmark's Setup() rebuilds the book and replays a 500k-event warmup
#   plus full batch pre-generation on every measured iteration.  A naive
#   recording would be dominated by that scaffolding (generate_pending_one,
#   build_book_from_tracking, RNG, tracking maps) and `perf report` would bury
#   the engine.  Instead we use perf's control FIFO: the runner enables
#   sampling only around the timed RunOp batch (see PerfRecordControl in
#   benchmark_runner.cpp), mirroring the PMC enable/disable window.
#
# The profiling binary is built as Release + `-g` with NO LLMES_PROFILE_*
# macros, so the annotated `add_limit_order` is the exact production code path
# measured by the campaign -- not an instrumented variant.
#
# Requirements: Linux perf, and typically `perf_event_paranoid <= 2`
# (sudo sysctl kernel.perf_event_paranoid=1).  Build with -g so annotate can
# map instructions back to source.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# --- Config (override via env) ---
BUILD_DIR="${BUILD_DIR:-$ROOT/build-perf}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark/results/hft_macro_perf_record_$(date +%Y%m%d_%H%M%S)}"

# Workload: keep aligned with the macro campaign, but use a large batch and
# enough iters so the *enabled* (sampled) window runs for several seconds.
# Each measured iter re-runs the heavy (unsampled) Setup, so prefer a big
# batch over many iters to keep wall time reasonable.
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-1000000}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ITERS="${ITERS:-40}"
SEED="${SEED:-42}"

# perf sampling
EVENTS="${EVENTS:-cycles,branch-misses}"
FREQ="${FREQ:-8000}"
CALL_GRAPH="${CALL_GRAPH:-dwarf}"
# Symbols to annotate (demangled substrings). Space-separated.
ANNOTATE_SYMBOLS="${ANNOTATE_SYMBOLS:-add_limit_order cancel_order modify_order add_market_order}"

VERSION_TAG="${VERSION_TAG:-perf_record}"
COMMIT_SHA="${COMMIT_SHA:-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)}"

BIN="$BUILD_DIR/benchmark/bench_hft_macro"
PERF_DATA="$OUT_DIR/perf.data"

# --- Preflight ---
if ! command -v perf >/dev/null 2>&1; then
	echo "ERROR: 'perf' not found on PATH. Install linux-tools / perf." >&2
	exit 1
fi

paranoid_file=/proc/sys/kernel/perf_event_paranoid
if [[ -r "$paranoid_file" ]]; then
	paranoid="$(cat "$paranoid_file")"
	if (( paranoid > 2 )); then
		echo "WARNING: kernel.perf_event_paranoid=$paranoid may block sampling." >&2
		echo "         Try: sudo sysctl kernel.perf_event_paranoid=1" >&2
	fi
fi

mkdir -p "$OUT_DIR"

# --- Build a dedicated profiling binary: Release + -g, NO profiling macros ---
# A separate build dir avoids clobbering the production Release build and
# guarantees debug line info without changing -O3 codegen.
echo "===== build (profiling binary) ====="
cmake -S "$ROOT" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
	-DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g" \
	-DLLMES_BUILD_BENCHMARKS=ON \
	-DLLMES_BUILD_TESTS=OFF \
	-DLLMES_PROFILE_HFT_MACRO_OPS=OFF \
	-DLLMES_PROFILE_HFT_MACRO_OP_PMCS=OFF
cmake --build "$BUILD_DIR" --target bench_hft_macro -j"$(nproc)"

if [[ ! -x "$BIN" ]]; then
	echo "ERROR: benchmark binary not found: $BIN" >&2
	exit 1
fi

# --- Set up the perf control FIFOs ---
FIFO_DIR="$(mktemp -d)"
CTL_FIFO="$FIFO_DIR/perf_ctl"
ACK_FIFO="$FIFO_DIR/perf_ack"
mkfifo "$CTL_FIFO" "$ACK_FIFO"

cleanup() {
	rm -rf "$FIFO_DIR"
}
trap cleanup EXIT

echo "===== perf record (window-isolated) ====="
echo "  bin          : $BIN"
echo "  events       : $EVENTS  (freq=$FREQ)"
echo "  orders/levels: $ORDERS / $LEVELS"
echo "  batch/iters  : $BATCH_SIZE / $ITERS  (warmup=$WARMUP_ITERS)"
echo "  out dir      : $OUT_DIR"
echo "  perf.data    : $PERF_DATA"

# `-D -1` starts with events disabled; the runner sends 'enable' via the
# control FIFO only around the RunOp batch. Env vars hand the FIFO paths to
# the child (inherited through perf).
LLMES_PERF_CTL_FIFO="$CTL_FIFO" \
LLMES_PERF_ACK_FIFO="$ACK_FIFO" \
perf record \
	-o "$PERF_DATA" \
	--control=fifo:"$CTL_FIFO","$ACK_FIFO" \
	-D -1 \
	-F "$FREQ" \
	-e "$EVENTS" \
	--call-graph "$CALL_GRAPH" \
	-- "$BIN" \
		--metric latency \
		--orders "$ORDERS" \
		--levels "$LEVELS" \
		--batch-size "$BATCH_SIZE" \
		--warmup-iters "$WARMUP_ITERS" \
		--iters "$ITERS" \
		--seed "$SEED" \
		--version-tag "$VERSION_TAG" \
		--commit-sha "$COMMIT_SHA" \
	2>&1 | tee "$OUT_DIR/run.log"

# --- Post-process: function-level report + instruction-level annotate ---
echo "===== perf report (enabled-window symbols) ====="
perf report -i "$PERF_DATA" --stdio --percent-limit 0.5 \
	> "$OUT_DIR/report.txt" 2>/dev/null || true
perf report -i "$PERF_DATA" --stdio --percent-limit 0.5 | head -60 || true

for sym in $ANNOTATE_SYMBOLS; do
	out="$OUT_DIR/annotate_${sym}.txt"
	# -l interleaves source lines; --stdio2 gives the richer asm+source view.
	if perf annotate -i "$PERF_DATA" --stdio2 --symbol="$sym" \
		> "$out" 2>/dev/null && [[ -s "$out" ]]; then
		echo "  annotated: $sym -> $out"
	else
		# Fall back to classic --stdio if --stdio2 produced nothing.
		perf annotate -i "$PERF_DATA" --stdio -l --symbol="$sym" \
			> "$out" 2>/dev/null || true
		[[ -s "$out" ]] && echo "  annotated: $sym -> $out" \
			|| echo "  (no samples for symbol: $sym)"
	fi
done

# --- Run metadata ---
{
	echo "scenario      : hft_macro"
	echo "version_tag   : $VERSION_TAG"
	echo "commit_sha    : $COMMIT_SHA"
	echo "build_type    : $BUILD_TYPE (+ -g, no LLMES_PROFILE_*)"
	echo "events        : $EVENTS"
	echo "freq          : $FREQ"
	echo "call_graph    : $CALL_GRAPH"
	echo "orders        : $ORDERS"
	echo "levels        : $LEVELS"
	echo "batch_size    : $BATCH_SIZE"
	echo "warmup_iters  : $WARMUP_ITERS"
	echo "iters         : $ITERS"
	echo "seed          : $SEED"
	echo "window        : RunOp batch only (perf --control=fifo, -D -1)"
} > "$OUT_DIR/meta.txt"

echo "done:"
echo "  report   : $OUT_DIR/report.txt"
echo "  annotate : $OUT_DIR/annotate_<symbol>.txt"
echo "  perf.data: $PERF_DATA"
echo "  meta     : $OUT_DIR/meta.txt"
