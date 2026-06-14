#!/usr/bin/env bash
# Build and compare baseline, LTO, PGO, and LTO+PGO variants of hft_macro.
#
# Examples:
#   bash benchmark/scripts/run_pgo_compare.sh
#   bash benchmark/scripts/run_pgo_compare.sh \
#     --modes baseline,baseline+lto,baseline+pgo,baseline+lto+pgo \
#     --trials 50
#   ENABLE_LINUX_ISOLATION=1 BENCH_CPU=auto \
#     bash benchmark/scripts/run_pgo_compare.sh --modes baseline,baseline+pgo
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

MODES_CSV="${MODES:-baseline,baseline+lto,baseline+pgo,baseline+lto+pgo}"
TRIALS="${TRIALS:-50}"
TRAIN_SEED_COUNT="${TRAIN_SEED_COUNT:-10}"
TRAIN_SEED_BASE="${TRAIN_SEED_BASE:-104729}"
VALIDATION_SEED_BASE="${VALIDATION_SEED_BASE:-1000000007}"
TRAIN_SEEDS_CSV="${TRAIN_SEEDS:-}"
VALIDATION_SEEDS_CSV="${VALIDATION_SEEDS:-}"

ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-100000}"
TRAIN_BATCH_SIZE="${TRAIN_BATCH_SIZE:-1000000}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ITERS="${ITERS:-1}"
TRAIN_WARMUP_ITERS="${TRAIN_WARMUP_ITERS:-1}"
TRAIN_ITERS="${TRAIN_ITERS:-1}"
METRICS_CSV="${METRICS:-latency,pmc}"

COMMON_FLAGS="${COMMON_FLAGS:--O3 -DNDEBUG -march=native}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
ENABLE_LINUX_ISOLATION="${ENABLE_LINUX_ISOLATION:-0}"
USE_CHRT_FIFO="${USE_CHRT_FIFO:-1}"
CXX_COMPILER="${CXX_COMPILER:-${CXX:-g++}}"

STAMP="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark/results/pgo_compare_${STAMP}}"
BUILD_ROOT="${BUILD_ROOT:-$ROOT/build-pgo-matrix}"
PROFILE_ROOT="${PROFILE_ROOT:-$OUT_DIR/profiles}"

usage() {
	cat <<'EOF'
Usage: run_pgo_compare.sh [options]

Options:
  --modes LIST              Comma-separated build modes. Accepted names:
                            baseline, baseline+lto, baseline+pgo,
                            baseline+lto+pgo (short: lto,pgo,lto+pgo)
  --trials N                Number of validation seeds/trials (default: 50)
  --train-seeds LIST        Explicit comma-separated PGO training seeds
  --validation-seeds LIST   Explicit comma-separated validation seeds
  --orders N                Initial order count (default: 100000)
  --levels N                Active price levels (default: 100)
  --batch-size N            Validation batch size (default: 100000)
  --train-batch-size N      PGO training batch size (default: 1000000)
  --metrics LIST            latency,pmc (default: both)
  --out-dir PATH            Result directory
  -h, --help                Show this help

The script uses 10 generated training seeds unless --train-seeds is supplied.
Validation uses one distinct seed per trial, and rejects overlap with training.
EOF
}

while (($#)); do
	case "$1" in
		--modes) MODES_CSV="$2"; shift 2 ;;
		--trials) TRIALS="$2"; shift 2 ;;
		--train-seeds) TRAIN_SEEDS_CSV="$2"; shift 2 ;;
		--validation-seeds) VALIDATION_SEEDS_CSV="$2"; shift 2 ;;
		--orders) ORDERS="$2"; shift 2 ;;
		--levels) LEVELS="$2"; shift 2 ;;
		--batch-size) BATCH_SIZE="$2"; shift 2 ;;
		--train-batch-size) TRAIN_BATCH_SIZE="$2"; shift 2 ;;
		--metrics) METRICS_CSV="$2"; shift 2 ;;
		--out-dir) OUT_DIR="$2"; PROFILE_ROOT="$2/profiles"; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
done

is_uint() { [[ "$1" =~ ^[0-9]+$ ]]; }
for value in "$TRIALS" "$TRAIN_SEED_COUNT" "$ORDERS" "$LEVELS" \
	"$BATCH_SIZE" "$TRAIN_BATCH_SIZE" "$WARMUP_ITERS" "$ITERS" \
	"$TRAIN_WARMUP_ITERS" "$TRAIN_ITERS"; do
	is_uint "$value" || { echo "ERROR: expected unsigned integer, got '$value'" >&2; exit 2; }
done
((TRIALS > 0)) || { echo "ERROR: --trials must be positive" >&2; exit 2; }
command -v "$CXX_COMPILER" >/dev/null 2>&1 || {
	echo "ERROR: C++ compiler not found: $CXX_COMPILER" >&2
	exit 2
}
"$CXX_COMPILER" --version | head -1 | grep -Eq 'g\+\+|GCC|GNU' || {
	echo "ERROR: this script currently supports GCC PGO only: $CXX_COMPILER" >&2
	exit 2
}

normalize_mode() {
	case "$1" in
		baseline) echo baseline ;;
		lto|baseline+lto) echo lto ;;
		pgo|baseline+pgo) echo pgo ;;
		lto+pgo|pgo+lto|baseline+lto+pgo|baseline+pgo+lto) echo lto_pgo ;;
		*) echo "ERROR: unknown build mode '$1'" >&2; return 1 ;;
	esac
}

IFS=',' read -r -a RAW_MODES <<< "$MODES_CSV"
MODES=()
for raw in "${RAW_MODES[@]}"; do
	raw="${raw//[[:space:]]/}"
	[[ -n "$raw" ]] || continue
	mode="$(normalize_mode "$raw")"
	seen=0
	for existing in "${MODES[@]}"; do
		[[ "$existing" == "$mode" ]] && seen=1
	done
	((seen == 0)) && MODES+=("$mode")
done
((${#MODES[@]} > 0)) || { echo "ERROR: no build modes selected" >&2; exit 2; }

generate_seeds() {
	local count="$1"
	local base="$2"
	local stride=1000003
	local i
	for ((i=0; i<count; ++i)); do
		echo $((base + i * stride))
	done
}

if [[ -n "$TRAIN_SEEDS_CSV" ]]; then
	IFS=',' read -r -a TRAIN_SEEDS <<< "$TRAIN_SEEDS_CSV"
else
	mapfile -t TRAIN_SEEDS < <(generate_seeds "$TRAIN_SEED_COUNT" "$TRAIN_SEED_BASE")
fi

if [[ -n "$VALIDATION_SEEDS_CSV" ]]; then
	IFS=',' read -r -a VALIDATION_SEEDS <<< "$VALIDATION_SEEDS_CSV"
	((${#VALIDATION_SEEDS[@]} == TRIALS)) || {
		echo "ERROR: --validation-seeds contains ${#VALIDATION_SEEDS[@]} seeds, expected $TRIALS" >&2
		exit 2
	}
else
	mapfile -t VALIDATION_SEEDS < <(generate_seeds "$TRIALS" "$VALIDATION_SEED_BASE")
fi

declare -A SEED_SET=()
for seed in "${TRAIN_SEEDS[@]}"; do
	is_uint "$seed" || { echo "ERROR: invalid training seed '$seed'" >&2; exit 2; }
	[[ -z "${SEED_SET[$seed]:-}" ]] || { echo "ERROR: duplicate training seed '$seed'" >&2; exit 2; }
	SEED_SET[$seed]=train
done
for seed in "${VALIDATION_SEEDS[@]}"; do
	is_uint "$seed" || { echo "ERROR: invalid validation seed '$seed'" >&2; exit 2; }
	[[ -z "${SEED_SET[$seed]:-}" ]] || {
		echo "ERROR: validation seed '$seed' overlaps or is duplicated" >&2
		exit 2
	}
	SEED_SET[$seed]=validation
done

IFS=',' read -r -a METRICS <<< "$METRICS_CSV"
for metric in "${METRICS[@]}"; do
	[[ "$metric" == latency || "$metric" == pmc ]] || {
		echo "ERROR: unsupported metric '$metric'" >&2
		exit 2
	}
done

mkdir -p "$OUT_DIR" "$BUILD_ROOT" "$PROFILE_ROOT" "$OUT_DIR/logs" "$OUT_DIR/bin"
COMMIT_SHA="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
LAT_CSV="$OUT_DIR/pgo_compare_latency_raw_trials.csv"
PMC_CSV="$OUT_DIR/pgo_compare_pmc_raw_trials.csv"
rm -f "$LAT_CSV" "$PMC_CSV"

{
	echo "training_seeds=${TRAIN_SEEDS[*]}"
	echo "validation_seeds=${VALIDATION_SEEDS[*]}"
} > "$OUT_DIR/seeds.txt"

mode_uses_lto() { [[ "$1" == lto || "$1" == lto_pgo ]]; }
mode_uses_pgo() { [[ "$1" == pgo || "$1" == lto_pgo ]]; }

configure_build() {
	local mode="$1"
	local stage="$2"
	local build_dir="$3"
	local profile_dir="$4"
	local compile_flags="$COMMON_FLAGS"
	local link_flags=""

	if mode_uses_lto "$mode"; then
		compile_flags+=" -flto"
		link_flags+=" -flto"
	fi
	if [[ "$stage" == generate ]]; then
		compile_flags+=" -fprofile-generate=$profile_dir -fprofile-exclude-files=benchmark_runner.cpp -DLLMES_PGO_TRAINING"
		link_flags+=" -fprofile-generate=$profile_dir"
	elif [[ "$stage" == use ]]; then
		compile_flags+=" -fprofile-use=$profile_dir -fprofile-exclude-files=benchmark_runner.cpp -fprofile-correction -Wno-missing-profile"
		link_flags+=" -fprofile-use=$profile_dir -fprofile-correction"
	fi

	cmake -S "$ROOT" -B "$build_dir" \
		-DCMAKE_CXX_COMPILER="$CXX_COMPILER" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_CXX_FLAGS_RELEASE="$compile_flags" \
		-DCMAKE_EXE_LINKER_FLAGS_RELEASE="$link_flags" \
		-DLLMES_BUILD_BENCHMARKS=ON \
		-DLLMES_BUILD_TESTS=OFF
}

run_training() {
	local bin="$1"
	local mode="$2"
	local i=0
	for seed in "${TRAIN_SEEDS[@]}"; do
		((++i))
		printf "  training %-8s [%2d/%2d] seed=%s\n" \
			"$mode" "$i" "${#TRAIN_SEEDS[@]}" "$seed"
		"$bin" \
			--metric latency \
			--trial-id "$i" \
			--orders "$ORDERS" \
			--levels "$LEVELS" \
			--batch-size "$TRAIN_BATCH_SIZE" \
			--warmup-iters "$TRAIN_WARMUP_ITERS" \
			--iters "$TRAIN_ITERS" \
			--seed "$seed" \
			--version-tag "${mode}_training" \
			--commit-sha "$COMMIT_SHA" >> "$OUT_DIR/logs/train_${mode}.log"
	done
}

echo "===== PGO/LTO build matrix ====="
echo "  modes            : ${MODES[*]}"
echo "  training seeds   : ${#TRAIN_SEEDS[@]}"
echo "  validation trials: $TRIALS"
echo "  output           : $OUT_DIR"

declare -A MODE_BIN=()
for mode in "${MODES[@]}"; do
	echo ""
	echo "--- Building $mode ---"
	build_dir="$BUILD_ROOT/$mode"
	profile_dir="$PROFILE_ROOT/$mode"
	rm -rf "$build_dir" "$profile_dir"
	mkdir -p "$profile_dir"

	if mode_uses_pgo "$mode"; then
		configure_build "$mode" generate "$build_dir" "$profile_dir"
		cmake --build "$build_dir" --target bench_hft_macro -j"$BUILD_JOBS"
		run_training "$build_dir/benchmark/bench_hft_macro" "$mode"
		find "$profile_dir" -type f -name '*.gcda' -print -quit | grep -q . || {
			echo "ERROR: PGO training produced no .gcda files for $mode" >&2
			exit 1
		}
		cmake --build "$build_dir" --target clean
		configure_build "$mode" use "$build_dir" "$profile_dir"
	else
		configure_build "$mode" normal "$build_dir" "$profile_dir"
	fi

	cmake --build "$build_dir" --target bench_hft_macro -j"$BUILD_JOBS" \
		2>&1 | tee "$OUT_DIR/logs/build_${mode}.log"
	cp "$build_dir/benchmark/bench_hft_macro" "$OUT_DIR/bin/bench_hft_macro_${mode}"
	MODE_BIN[$mode]="$OUT_DIR/bin/bench_hft_macro_${mode}"
	size "${MODE_BIN[$mode]}" > "$OUT_DIR/logs/size_${mode}.txt" 2>/dev/null || true
done

RUN_PREFIX=()
ISOLATION_LIB="$ROOT/benchmark/scripts/lib/bench_linux_isolation.sh"
if [[ "$ENABLE_LINUX_ISOLATION" == 1 ]]; then
	[[ -f "$ISOLATION_LIB" ]] || { echo "ERROR: missing $ISOLATION_LIB" >&2; exit 1; }
	# shellcheck source=/dev/null
	source "$ISOLATION_LIB"
	bench_linux_isolation_begin "$OUT_DIR/system"
	RUN_PREFIX=("${BENCH_LINUX_RUN_PREFIX[@]}")
fi

cleanup() {
	if [[ "$ENABLE_LINUX_ISOLATION" == 1 ]] && declare -F bench_linux_isolation_end >/dev/null; then
		bench_linux_isolation_end || true
	fi
}
trap cleanup EXIT

echo ""
echo "===== Validation ====="
mode_count="${#MODES[@]}"
for ((trial=1; trial<=TRIALS; ++trial)); do
	seed="${VALIDATION_SEEDS[$((trial - 1))]}"
	# Rotate build order each trial so no mode always runs first or last.
	offset=$(((trial - 1) % mode_count))
	for ((step=0; step<mode_count; ++step)); do
		mode="${MODES[$(((offset + step) % mode_count))]}"
		bin="${MODE_BIN[$mode]}"
		for metric in "${METRICS[@]}"; do
			if [[ "$metric" == latency ]]; then
				out_csv="$LAT_CSV"
			else
				out_csv="$PMC_CSV"
			fi
			printf "  trial=%-3d seed=%-12s mode=%-8s metric=%s\n" \
				"$trial" "$seed" "$mode" "$metric"
			"${RUN_PREFIX[@]}" "$bin" \
				--metric "$metric" \
				--trial-id "$trial" \
				--orders "$ORDERS" \
				--levels "$LEVELS" \
				--batch-size "$BATCH_SIZE" \
				--warmup-iters "$WARMUP_ITERS" \
				--iters "$ITERS" \
				--seed "$seed" \
				--version-tag "$mode" \
				--commit-sha "$COMMIT_SHA" \
				--out "$out_csv" >> "$OUT_DIR/logs/validation.log"
		done
	done
done

echo ""
echo "===== Merge and plots ====="
merge_args=(
	LAT_CSV="$LAT_CSV"
	PMC_CSV="$PMC_CSV"
	MERGED_RAW_OUT="$OUT_DIR/pgo_compare_merged_raw_trials.csv"
	MERGED_AGG_OUT="$OUT_DIR/pgo_compare_merged_agg.csv"
	OUT_PREFIX=pgo_compare
	GROUP_BY_SEED=0
)
env "${merge_args[@]}" python3 "$ROOT/benchmark/scripts/merge_benchmark_metrics.py"

if python3 -c 'import matplotlib, pandas, numpy' >/dev/null 2>&1; then
	env \
		AGG_CSV="$OUT_DIR/pgo_compare_merged_agg.csv" \
		PLOT_OUT_DIR="$OUT_DIR/plots" \
		OUT_PREFIX=pgo_compare \
		PLOT_METRICS="avg_ns,ops_s,cycles_per_op,instructions_per_op,branch_misses_per_op,cpi" \
		PLOT_LEVEL="$LEVELS" \
		FIXED_ORDERS="$ORDERS" \
		LOGX=0 \
		python3 "$ROOT/benchmark/scripts/plot_version_comparison.py"
else
	echo "  plotting skipped: matplotlib/pandas/numpy not available"
fi

{
	echo "timestamp=$(date -Iseconds)"
	echo "commit=$COMMIT_SHA"
	echo "modes=${MODES[*]}"
	echo "trials=$TRIALS"
	echo "training_seed_count=${#TRAIN_SEEDS[@]}"
	echo "validation_seed_count=${#VALIDATION_SEEDS[@]}"
	echo "orders=$ORDERS"
	echo "levels=$LEVELS"
	echo "batch_size=$BATCH_SIZE"
	echo "train_batch_size=$TRAIN_BATCH_SIZE"
	echo "metrics=$METRICS_CSV"
	echo "common_flags=$COMMON_FLAGS"
	echo "linux_isolation=$ENABLE_LINUX_ISOLATION"
	if [[ "$ENABLE_LINUX_ISOLATION" == 1 ]]; then
		bench_linux_isolation_write_env /dev/stdout
	fi
	echo
	"$CXX_COMPILER" --version | head -1
	cmake --version | head -1
} > "$OUT_DIR/env.txt"

echo ""
echo "Done."
echo "  Aggregate: $OUT_DIR/pgo_compare_merged_agg.csv"
echo "  Raw data : $OUT_DIR/pgo_compare_merged_raw_trials.csv"
echo "  Seeds    : $OUT_DIR/seeds.txt"
echo "  Plots    : $OUT_DIR/plots/"
