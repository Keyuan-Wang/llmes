#!/usr/bin/env bash
#
# hft_macro_perf_stat.sh
#
# Window-isolated `perf stat` for TLB / page-fault counters on the hft_macro
# engine hot path.  Uses the same FIFO control protocol as perf record so that
# only the measured RunOp batch is counted (see PerfWindowControl in
# benchmark_runner.cpp).
#
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$SCRIPTS_DIR/../.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$ROOT/build-perf-stat}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark/results/hft_macro_perf_stat_$(date +%Y%m%d_%H%M%S)}"

ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-1000000}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ITERS="${ITERS:-40}"
SEED="${SEED:-42}"

# Comma-separated perf stat events.  Override if your PMU lacks some names.
# L1-dcache-loads is the portable denominator for dTLB miss rate (dTLB-loads is
# unreliable on AMD — it counts misses, not accesses).
EVENTS="${EVENTS:-L1-dcache-loads,dTLB-load-misses,iTLB-load-misses,page-faults,minor-faults,major-faults,instructions,cycles}"
REPEAT="${REPEAT:-1}"

VERSION_TAG="${VERSION_TAG:-perf_stat_tlb_pf}"
COMMIT_SHA="${COMMIT_SHA:-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)}"

ENABLE_LINUX_ISOLATION="${ENABLE_LINUX_ISOLATION:-1}"
BENCH_CPU="${BENCH_CPU:-auto}"
NUMA_NODE="${NUMA_NODE:-auto}"
CPU_PROBE_SECONDS="${CPU_PROBE_SECONDS:-2}"
USE_NUMACTL="${USE_NUMACTL:-1}"
SET_PERFORMANCE_GOVERNOR="${SET_PERFORMANCE_GOVERNOR:-1}"
RESTORE_GOVERNOR="${RESTORE_GOVERNOR:-0}"
REDUCE_BACKGROUND_NOISE="${REDUCE_BACKGROUND_NOISE:-1}"
STOP_NOISY_TIMERS="${STOP_NOISY_TIMERS:-1}"
AVOID_IRQ_ON_BENCH_CPU="${AVOID_IRQ_ON_BENCH_CPU:-1}"
AVOID_IRQ_ON_SMT_SIBLINGS="${AVOID_IRQ_ON_SMT_SIBLINGS:-1}"
RESTORE_IRQ_AFFINITY="${RESTORE_IRQ_AFFINITY:-0}"
AGGRESSIVE_ISOLATION="${AGGRESSIVE_ISOLATION:-1}"
PIN_WORKQUEUES_AWAY="${PIN_WORKQUEUES_AWAY:-1}"
STOP_IRQBALANCE="${STOP_IRQBALANCE:-1}"
DISABLE_KERNEL_WATCHDOGS="${DISABLE_KERNEL_WATCHDOGS:-1}"
LOCK_CPU_DMA_LATENCY="${LOCK_CPU_DMA_LATENCY:-1}"
USE_CHRT_FIFO="${USE_CHRT_FIFO:-0}"
REALTIME_PRIORITY="${REALTIME_PRIORITY:-95}"

BIN="$BUILD_DIR/benchmark/bench_hft_macro"
STAT_OUT="$OUT_DIR/perf_stat.txt"
ISOLATION_LIB="$SCRIPTS_DIR/lib/bench_linux_isolation.sh"

if ! command -v perf >/dev/null 2>&1; then
	echo "ERROR: 'perf' not found on PATH." >&2
	exit 1
fi

paranoid_file=/proc/sys/kernel/perf_event_paranoid
if [[ -r "$paranoid_file" ]]; then
	paranoid="$(cat "$paranoid_file")"
	if (( paranoid > 2 )); then
		echo "WARNING: kernel.perf_event_paranoid=$paranoid may block counters." >&2
		echo "         Try: sudo sysctl kernel.perf_event_paranoid=1" >&2
	fi
fi

mkdir -p "$OUT_DIR"

RUN_PREFIX=()
if [[ "$ENABLE_LINUX_ISOLATION" == "1" ]]; then
	# shellcheck source=/dev/null
	source "$ISOLATION_LIB"
	bench_linux_isolation_begin "$OUT_DIR"
	RUN_PREFIX=("${BENCH_LINUX_RUN_PREFIX[@]}")
fi

isolation_cleanup() {
	if [[ "$ENABLE_LINUX_ISOLATION" == "1" ]] && declare -F bench_linux_isolation_end >/dev/null; then
		bench_linux_isolation_end || true
	fi
}

echo "===== build (stat binary) ====="
cmake -S "$ROOT" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
	-DLLMES_BUILD_BENCHMARKS=ON \
	-DLLMES_BUILD_TESTS=OFF
cmake --build "$BUILD_DIR" --target bench_hft_macro -j"$(nproc)"

if [[ ! -x "$BIN" ]]; then
	echo "ERROR: benchmark binary not found: $BIN" >&2
	exit 1
fi

FIFO_DIR="$(mktemp -d)"
CTL_FIFO="$FIFO_DIR/perf_ctl"
ACK_FIFO="$FIFO_DIR/perf_ack"
mkfifo "$CTL_FIFO" "$ACK_FIFO"

cleanup() {
	isolation_cleanup
	rm -rf "$FIFO_DIR"
}
trap cleanup EXIT

measured_ops=$(( BATCH_SIZE * ITERS ))

echo "===== perf stat (window-isolated) ====="
echo "  bin          : $BIN"
echo "  events       : $EVENTS"
echo "  orders/levels: $ORDERS / $LEVELS"
echo "  batch/iters  : $BATCH_SIZE / $ITERS  (warmup=$WARMUP_ITERS)"
echo "  measured_ops : $measured_ops"
echo "  out dir      : $OUT_DIR"
if [[ "$ENABLE_LINUX_ISOLATION" == "1" ]]; then
	echo "  isolation    : enabled (bench_cpu=${BENCH_CPU_SELECTED:-pending})"
fi

IFS=',' read -r -a EVENT_ARR <<< "$EVENTS"
PERF_EVENT_ARGS=()
for ev in "${EVENT_ARR[@]}"; do
	PERF_EVENT_ARGS+=(-e "$ev")
done

LLMES_PERF_CTL_FIFO="$CTL_FIFO" \
LLMES_PERF_ACK_FIFO="$ACK_FIFO" \
"${RUN_PREFIX[@]}" perf stat \
	-o "$STAT_OUT" \
	--control=fifo:"$CTL_FIFO","$ACK_FIFO" \
	-D -1 \
	-r "$REPEAT" \
	"${PERF_EVENT_ARGS[@]}" \
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

python3 - "$STAT_OUT" "$OUT_DIR/summary.txt" "$measured_ops" <<'PY'
import re
import sys
from pathlib import Path

stat_path, summary_path, measured_ops_s = sys.argv[1:4]
measured_ops = int(measured_ops_s)
text = Path(stat_path).read_text(errors="ignore")

def parse_counter(name):
    # perf stat lines: "     1,234,567      dTLB-load-misses"
    pat = re.compile(
        rf"^\s*([\d,]+(?:\.\d+)?(?:\s*[KMGT]?)?)\s+{re.escape(name)}\s*(?:\([^)]*\))?\s*$",
        re.MULTILINE,
    )
    m = pat.search(text)
    if not m:
        return None
    raw = m.group(1).replace(",", "")
    mult = 1.0
    if raw.endswith("K"):
        mult = 1e3
        raw = raw[:-1]
    elif raw.endswith("M"):
        mult = 1e6
        raw = raw[:-1]
    elif raw.endswith("G"):
        mult = 1e9
        raw = raw[:-1]
    elif raw.endswith("T"):
        mult = 1e12
        raw = raw[:-1]
    try:
        return float(raw) * mult
    except ValueError:
        return None

counters = [
    "L1-dcache-loads",
    "dTLB-load-misses",
    "dTLB-loads",
    "dTLB-stores",
    "dTLB-store-misses",
    "iTLB-load-misses",
    "page-faults",
    "minor-faults",
    "major-faults",
    "cycles",
    "instructions",
]
values = {name: parse_counter(name) for name in counters}

def fmt_rate(num, den):
    if num is None or den is None or den == 0:
        return "NA"
    return f"{num / den:.6f}"

def fmt_mpki(misses, instr):
    if misses is None or instr is None or instr == 0:
        return "NA"
    return f"{misses / instr * 1000.0:.6f}"

lines = [
    f"measured_ops={measured_ops}",
    "window=RunOp batch only (perf stat --control=fifo, -D -1)",
    "",
    "counter\traw\tper_op\trate",
]
for name in counters:
    v = values.get(name)
    if v is None:
        lines.append(f"{name}\tNA\tNA\tNA")
        continue
    per_op = v / measured_ops if measured_ops else 0.0
    rate = ""
    if name == "dTLB-load-misses":
        rate = fmt_rate(v, values.get("L1-dcache-loads"))
    elif name == "dTLB-store-misses":
        rate = fmt_rate(v, values.get("L1-dcache-stores"))
    lines.append(f"{name}\t{v:.0f}\t{per_op:.6f}\t{rate}")

lines.extend([
    "",
    "derived_metric\tvalue\tnote",
    f"dtlb_miss_rate\t{fmt_rate(values.get('dTLB-load-misses'), values.get('L1-dcache-loads'))}\tdTLB-load-misses / L1-dcache-loads",
    f"dtlb_mpki\t{fmt_mpki(values.get('dTLB-load-misses'), values.get('instructions'))}\tmisses per 1K instructions",
    f"itlb_mpki\t{fmt_mpki(values.get('iTLB-load-misses'), values.get('instructions'))}\tmisses per 1K instructions",
    f"page_faults_per_op\t{(values.get('page-faults') or 0) / measured_ops if measured_ops else 'NA'}\tpage-faults / measured_ops",
    f"minor_faults_per_op\t{(values.get('minor-faults') or 0) / measured_ops if measured_ops else 'NA'}\tminor-faults / measured_ops",
    f"major_faults_per_op\t{(values.get('major-faults') or 0) / measured_ops if measured_ops else 'NA'}\tmajor-faults / measured_ops",
])

Path(summary_path).write_text("\n".join(lines) + "\n")
print(Path(summary_path).read_text())
PY

{
	echo "scenario      : hft_macro"
	echo "version_tag   : $VERSION_TAG"
	echo "commit_sha    : $COMMIT_SHA"
	echo "events        : $EVENTS"
	echo "repeat        : $REPEAT"
	echo "orders        : $ORDERS"
	echo "levels        : $LEVELS"
	echo "batch_size    : $BATCH_SIZE"
	echo "warmup_iters  : $WARMUP_ITERS"
	echo "iters         : $ITERS"
	echo "seed          : $SEED"
	echo "measured_ops  : $measured_ops"
	echo "window        : RunOp batch only (perf stat --control=fifo, -D -1)"
	echo "linux_isolation: $ENABLE_LINUX_ISOLATION"
} > "$OUT_DIR/meta.txt"
if [[ "$ENABLE_LINUX_ISOLATION" == "1" ]] && declare -F bench_linux_isolation_write_env >/dev/null; then
	bench_linux_isolation_write_env "$OUT_DIR/meta.txt"
fi

echo "done:"
echo "  perf_stat : $STAT_OUT"
echo "  summary   : $OUT_DIR/summary.txt"
echo "  meta      : $OUT_DIR/meta.txt"
