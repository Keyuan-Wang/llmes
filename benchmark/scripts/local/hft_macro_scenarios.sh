#!/usr/bin/env bash
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$SCRIPTS_DIR/../.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark/results}"
mkdir -p "$OUT_DIR"

DRY_RUN="${DRY_RUN:-0}"
if [[ "${1:-}" == "--dry-run" ]]; then
	DRY_RUN=1
fi

BUILD_TYPE="${BUILD_TYPE:-Release}"
ENABLE_LTO="${ENABLE_LTO:-0}"
BUILD_DIR="${BUILD_DIR:-}"
if [[ -z "$BUILD_DIR" ]]; then
	if [[ "$ENABLE_LTO" == "1" ]]; then
		BUILD_DIR="$ROOT/build-lto"
	else
		BUILD_DIR="$ROOT/build"
	fi
fi
TRIALS="${TRIALS:-10}"
ITERS="${ITERS:-1}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-100000}"
SEED="${SEED:-42}"
FOCUS="${FOCUS:-all}"
VERSION_TAG="${VERSION_TAG:-baseline}"
COMMIT_SHA="${COMMIT_SHA:-unknown}"
OUT_CSV="${OUT_CSV:-$OUT_DIR/hft_macro_scenario_calls.csv}"

# Optional Linux isolation (sourced from lib/bench_linux_isolation.sh).
# Set ENABLE_LINUX_ISOLATION=1 to activate.
ENABLE_LINUX_ISOLATION="${ENABLE_LINUX_ISOLATION:-0}"
ISOLATION_LIB="$SCRIPTS_DIR/lib/bench_linux_isolation.sh"

if (( DRY_RUN == 0 )); then
	CXX_FLAGS_RELEASE="-O3 -DNDEBUG"
	LINK_FLAGS_RELEASE=""
	if [[ "$ENABLE_LTO" == "1" ]]; then
		CXX_FLAGS_RELEASE+=" -march=native -flto"
		LINK_FLAGS_RELEASE="-flto"
	fi
	cmake -S "$ROOT" -B "$BUILD_DIR" \
		-DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
		-DCMAKE_CXX_FLAGS_RELEASE="$CXX_FLAGS_RELEASE" \
		-DCMAKE_EXE_LINKER_FLAGS_RELEASE="$LINK_FLAGS_RELEASE" \
		-DLLME_BUILD_BENCHMARKS=ON
	cmake --build "$BUILD_DIR" -j
	: > "$OUT_CSV"
fi

BIN="$BUILD_DIR/benchmark/bench_hft_macro_scenarios"

# --- Linux isolation setup ---
RUN_PREFIX=()
if [[ "$ENABLE_LINUX_ISOLATION" == "1" && "$DRY_RUN" == "0" ]]; then
	if [[ ! -f "$ISOLATION_LIB" ]]; then
		echo "ERROR: isolation library not found: $ISOLATION_LIB" >&2
		exit 1
	fi
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
trap isolation_cleanup EXIT

echo "===== HFT macro per-scenario call data ====="
echo "  trials      : $TRIALS"
echo "  enable_lto  : $ENABLE_LTO"
echo "  build_dir   : $BUILD_DIR"
echo "  focus       : $FOCUS"
echo "  batch_size  : $BATCH_SIZE"
echo "  iters       : $ITERS"
echo "  warmup_iters: $WARMUP_ITERS"
echo "  isolation   : $ENABLE_LINUX_ISOLATION"
echo "  out         : $OUT_CSV"
echo ""

for trial in $(seq 1 "$TRIALS"); do
	printf "[%3d/%3d] focus=%-12s batch=%-8s " \
		"$trial" "$TRIALS" "$FOCUS" "$BATCH_SIZE"

	if (( DRY_RUN == 1 )); then
		echo "(dry-run)"
		continue
	fi

	if "${RUN_PREFIX[@]}" "$BIN" \
		--trial-id "$trial" \
		--orders "$ORDERS" \
		--levels "$LEVELS" \
		--batch-size "$BATCH_SIZE" \
		--warmup-iters "$WARMUP_ITERS" \
		--iters "$ITERS" \
		--seed "$SEED" \
		--focus "$FOCUS" \
		--version-tag "$VERSION_TAG" \
		--commit-sha "$COMMIT_SHA" \
		--out "$OUT_CSV" > /dev/null; then
		echo "ok"
	else
		echo "FAIL"
		exit 1
	fi
done

if (( DRY_RUN == 1 )); then
	echo ""
	echo "Dry-run complete. $TRIALS commands would be executed."
	exit 0
fi

echo ""
echo "scenario call data saved: $OUT_CSV"

# Write run metadata
{
	echo "timestamp=$(date -Iseconds)"
	echo "commit=$COMMIT_SHA"
	echo "trials=$TRIALS"
	echo "focus=$FOCUS"
	echo "seed=$SEED"
	echo "orders=$ORDERS"
	echo "levels=$LEVELS"
	echo "batch_size=$BATCH_SIZE"
	echo "iters=$ITERS"
	echo "warmup_iters=$WARMUP_ITERS"
	echo "version_tag=$VERSION_TAG"
	echo "enable_lto=$ENABLE_LTO"
	echo "build_dir=$BUILD_DIR"
	if [[ "$ENABLE_LINUX_ISOLATION" == "1" ]] && declare -F bench_linux_isolation_write_env >/dev/null; then
		bench_linux_isolation_write_env /dev/stdout
	fi
	echo
	uname -a || true
	echo
	lscpu || true
} > "$OUT_DIR/env.txt"
