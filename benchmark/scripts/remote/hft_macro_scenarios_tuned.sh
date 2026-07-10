#!/usr/bin/env bash
set -euo pipefail

# Remote HFT per-scenario runner with Linux measurement hygiene.
# Linux isolation is delegated to local/hft_macro_scenarios.sh +
# lib/bench_linux_isolation.sh (synced to the remote before the run).

SERVER_IP="${SERVER_IP:-}"
SSH_USER="${SSH_USER:-root}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519_hetzner}"
SSH_PORT="${SSH_PORT:-22}"

REPO_URL="${REPO_URL:-}"
BRANCH="${BRANCH:-master}"
COMMIT_SHA="${COMMIT_SHA:-}"

REMOTE_ROOT="${REMOTE_ROOT:-/root/llmes-bench}"
REMOTE_REPO_DIR="${REMOTE_REPO_DIR:-$REMOTE_ROOT/repo}"
REMOTE_ARTIFACTS_DIR="${REMOTE_ARTIFACTS_DIR:-$REMOTE_ROOT/artifacts}"
REMOTE_TARBALL="${REMOTE_TARBALL:-$REMOTE_ROOT/hft_macro_scenarios_tuned_artifacts.tgz}"

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(cd "$SCRIPTS_DIR/../.." && pwd)"
LOCAL_OUT_ROOT="${LOCAL_OUT_DIR:-$ROOT_DIR/server_results}"
LOCAL_OUT_ROOT="$(mkdir -p "$LOCAL_OUT_ROOT" && cd "$LOCAL_OUT_ROOT" && pwd)"
LOCAL_ARCHIVES_DIR="$LOCAL_OUT_ROOT/archives"
LOCAL_RUNS_DIR="$LOCAL_OUT_ROOT/hft_macro/scenarios_tuned"
mkdir -p "$LOCAL_ARCHIVES_DIR" "$LOCAL_RUNS_DIR"

# Benchmark params
TRIALS="${TRIALS:-10}"
ITERS="${ITERS:-1}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-100000}"
SEED="${SEED:-42}"
FOCUS="${FOCUS:-all}"
VERSION_TAG="${VERSION_TAG:-tuned}"
ENABLE_LTO="${ENABLE_LTO:-0}"
BUILD_DIR="${BUILD_DIR:-}"

# Isolation knobs (forwarded to local/hft_macro_scenarios.sh via ENABLE_LINUX_ISOLATION=1)
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
USE_CHRT_FIFO="${USE_CHRT_FIFO:-1}"
REALTIME_PRIORITY="${REALTIME_PRIORITY:-95}"

INSTALL_DEPS="${INSTALL_DEPS:-1}"
# Sync local scripts to remote before running so local edits take effect
# without requiring a git push.
SYNC_LOCAL_SCRIPTS="${SYNC_LOCAL_SCRIPTS:-1}"
REMOTE_SYNC_TARBALL="${REMOTE_SYNC_TARBALL:-$REMOTE_ROOT/hft_macro_scenarios_scripts_sync.tgz}"

if [[ -z "$SERVER_IP" || -z "$REPO_URL" ]]; then
	echo "Usage: SERVER_IP=1.2.3.4 REPO_URL=git@github.com:you/llmes.git $0" >&2
	exit 1
fi

SSH_OPTS=(-i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=accept-new)
SCP_OPTS=(-i "$SSH_KEY" -P "$SSH_PORT" -o StrictHostKeyChecking=accept-new)
STAMP="$(date +%Y%m%d_%H%M%S)"
TAG_SAFE="$(echo "${COMMIT_SHA:-$BRANCH}" | tr '/:' '__')"
LOCAL_TARBALL="$LOCAL_ARCHIVES_DIR/hft_macro_scenarios_tuned_${TAG_SAFE}_${STAMP}.tgz"

echo "===== Remote tuned HFT macro per-scenario pipeline ====="
echo "  Server     : ${SSH_USER}@${SERVER_IP}"
echo "  Checkout   : ${COMMIT_SHA:-$BRANCH}"
echo "  Trials     : $TRIALS"
echo "  Batch size : $BATCH_SIZE"
echo "  Bench CPU  : $BENCH_CPU"
echo "  LTO        : $ENABLE_LTO"
echo ""

# Sync lib + local scenario script so remote picks up any local edits.
if [[ "$SYNC_LOCAL_SCRIPTS" == "1" ]]; then
	LOCAL_SYNC_TAR="$(mktemp /tmp/hft_scenarios_scripts_XXXXXX.tgz)"
	tar czf "$LOCAL_SYNC_TAR" -C "$ROOT_DIR" \
		benchmark/scripts/lib/bench_linux_isolation.sh \
		benchmark/scripts/local/hft_macro_scenarios.sh
	ssh "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}" "mkdir -p '$REMOTE_ROOT'"
	scp "${SCP_OPTS[@]}" "$LOCAL_SYNC_TAR" "${SSH_USER}@${SERVER_IP}:${REMOTE_SYNC_TARBALL}"
	rm -f "$LOCAL_SYNC_TAR"
	echo "  Synced lib + local scenario scripts to remote"
fi

ssh "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}" \
	"REPO_URL='$REPO_URL' BRANCH='$BRANCH' COMMIT_SHA='$COMMIT_SHA' \
	REMOTE_ROOT='$REMOTE_ROOT' REMOTE_REPO_DIR='$REMOTE_REPO_DIR' \
	REMOTE_ARTIFACTS_DIR='$REMOTE_ARTIFACTS_DIR' REMOTE_TARBALL='$REMOTE_TARBALL' \
	TRIALS='$TRIALS' ITERS='$ITERS' WARMUP_ITERS='$WARMUP_ITERS' \
	ORDERS='$ORDERS' LEVELS='$LEVELS' BATCH_SIZE='$BATCH_SIZE' \
	SEED='$SEED' FOCUS='$FOCUS' VERSION_TAG='$VERSION_TAG' \
	ENABLE_LTO='$ENABLE_LTO' BUILD_DIR='$BUILD_DIR' \
	BENCH_CPU='$BENCH_CPU' NUMA_NODE='$NUMA_NODE' \
	CPU_PROBE_SECONDS='$CPU_PROBE_SECONDS' USE_NUMACTL='$USE_NUMACTL' \
	SET_PERFORMANCE_GOVERNOR='$SET_PERFORMANCE_GOVERNOR' RESTORE_GOVERNOR='$RESTORE_GOVERNOR' \
	REDUCE_BACKGROUND_NOISE='$REDUCE_BACKGROUND_NOISE' STOP_NOISY_TIMERS='$STOP_NOISY_TIMERS' \
	AVOID_IRQ_ON_BENCH_CPU='$AVOID_IRQ_ON_BENCH_CPU' AVOID_IRQ_ON_SMT_SIBLINGS='$AVOID_IRQ_ON_SMT_SIBLINGS' \
	RESTORE_IRQ_AFFINITY='$RESTORE_IRQ_AFFINITY' AGGRESSIVE_ISOLATION='$AGGRESSIVE_ISOLATION' \
	PIN_WORKQUEUES_AWAY='$PIN_WORKQUEUES_AWAY' STOP_IRQBALANCE='$STOP_IRQBALANCE' \
	DISABLE_KERNEL_WATCHDOGS='$DISABLE_KERNEL_WATCHDOGS' LOCK_CPU_DMA_LATENCY='$LOCK_CPU_DMA_LATENCY' \
	USE_CHRT_FIFO='$USE_CHRT_FIFO' REALTIME_PRIORITY='$REALTIME_PRIORITY' \
	INSTALL_DEPS='$INSTALL_DEPS' \
	REMOTE_SYNC_TARBALL='$REMOTE_SYNC_TARBALL' bash -s" <<'ENDSSH'
set -euo pipefail

mkdir -p "$REMOTE_ROOT" "$REMOTE_ARTIFACTS_DIR"

# ---- 1. System dependencies ----
if [[ "$INSTALL_DEPS" == "1" ]]; then
	export DEBIAN_FRONTEND=noninteractive
	apt-get update
	apt-get install -y --no-install-recommends \
		git ca-certificates build-essential cmake python3 python3-venv python3-pip \
		numactl util-linux
	apt-get install -y --no-install-recommends linux-tools-common || true
fi

# ---- 2. Clone / fetch repo ----
if [[ ! -d "$REMOTE_REPO_DIR/.git" ]]; then
	git clone "$REPO_URL" "$REMOTE_REPO_DIR"
fi
cd "$REMOTE_REPO_DIR"
git fetch --all --prune --tags
if [[ -n "$COMMIT_SHA" ]]; then
	git fetch origin "$COMMIT_SHA" 2>/dev/null || true
	if git show-ref --verify --quiet "refs/remotes/origin/$COMMIT_SHA"; then
		git reset --hard "origin/$COMMIT_SHA"
	elif git rev-parse --verify "$COMMIT_SHA^{commit}" >/dev/null 2>&1; then
		git reset --hard "$COMMIT_SHA"
	else
		echo "ERROR: unknown ref: $COMMIT_SHA" >&2; exit 1
	fi
else
	git checkout "$BRANCH"
	git reset --hard "origin/$BRANCH"
fi

# ---- 3. Apply synced scripts (lib + local) ----
if [[ -f "$REMOTE_SYNC_TARBALL" ]]; then
	tar -xzf "$REMOTE_SYNC_TARBALL" -C "$REMOTE_REPO_DIR"
	rm -f "$REMOTE_SYNC_TARBALL"
fi

# ---- 4. Python venv ----
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt

# ---- 5. Build ----
RESOLVED_BUILD_DIR="${BUILD_DIR:-}"
if [[ -z "$RESOLVED_BUILD_DIR" ]]; then
	RESOLVED_BUILD_DIR="$([[ "$ENABLE_LTO" == "1" ]] && echo build-lto || echo build)"
fi
CXX_FLAGS_RELEASE="-O3 -DNDEBUG"
LINK_FLAGS_RELEASE=""
if [[ "$ENABLE_LTO" == "1" ]]; then
	CXX_FLAGS_RELEASE+=" -march=native -flto"
	LINK_FLAGS_RELEASE="-flto"
fi
cmake -S . -B "$RESOLVED_BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_CXX_FLAGS_RELEASE="$CXX_FLAGS_RELEASE" \
	-DCMAKE_EXE_LINKER_FLAGS_RELEASE="$LINK_FLAGS_RELEASE" \
	-DLLME_BUILD_BENCHMARKS=ON
cmake --build "$RESOLVED_BUILD_DIR" -j"$(nproc)"
ctest --test-dir "$RESOLVED_BUILD_DIR" --output-on-failure | tee "$REMOTE_ARTIFACTS_DIR/ctest.log"

# ---- 6. Per-scenario benchmark with Linux isolation ----
RESULTS_DIR="$REMOTE_REPO_DIR/benchmark/results/hft_macro_scenarios_tuned_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"
OUT_CSV="$RESULTS_DIR/hft_macro_scenario_cycles.csv"
OUT_PNG="$RESULTS_DIR/hft_macro_scenario_cycles_distributions.png"

env \
	ENABLE_LINUX_ISOLATION=1 \
	BENCH_CPU="$BENCH_CPU" NUMA_NODE="$NUMA_NODE" \
	CPU_PROBE_SECONDS="$CPU_PROBE_SECONDS" \
	USE_NUMACTL="$USE_NUMACTL" SET_PERFORMANCE_GOVERNOR="$SET_PERFORMANCE_GOVERNOR" \
	RESTORE_GOVERNOR="$RESTORE_GOVERNOR" REDUCE_BACKGROUND_NOISE="$REDUCE_BACKGROUND_NOISE" \
	STOP_NOISY_TIMERS="$STOP_NOISY_TIMERS" AVOID_IRQ_ON_BENCH_CPU="$AVOID_IRQ_ON_BENCH_CPU" \
	AVOID_IRQ_ON_SMT_SIBLINGS="$AVOID_IRQ_ON_SMT_SIBLINGS" \
	RESTORE_IRQ_AFFINITY="$RESTORE_IRQ_AFFINITY" \
	AGGRESSIVE_ISOLATION="$AGGRESSIVE_ISOLATION" PIN_WORKQUEUES_AWAY="$PIN_WORKQUEUES_AWAY" \
	STOP_IRQBALANCE="$STOP_IRQBALANCE" DISABLE_KERNEL_WATCHDOGS="$DISABLE_KERNEL_WATCHDOGS" \
	LOCK_CPU_DMA_LATENCY="$LOCK_CPU_DMA_LATENCY" USE_CHRT_FIFO="$USE_CHRT_FIFO" \
	REALTIME_PRIORITY="$REALTIME_PRIORITY" \
	TRIALS="$TRIALS" ITERS="$ITERS" WARMUP_ITERS="$WARMUP_ITERS" \
	ORDERS="$ORDERS" LEVELS="$LEVELS" BATCH_SIZE="$BATCH_SIZE" \
	SEED="$SEED" FOCUS="$FOCUS" VERSION_TAG="$VERSION_TAG" \
	ENABLE_LTO="$ENABLE_LTO" BUILD_DIR="$REMOTE_REPO_DIR/$RESOLVED_BUILD_DIR" \
	COMMIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" \
	OUT_DIR="$RESULTS_DIR" OUT_CSV="$OUT_CSV" \
	bash benchmark/scripts/local/hft_macro_scenarios.sh \
	| tee "$REMOTE_ARTIFACTS_DIR/run_hft_macro_scenarios_tuned.log"

# ---- 7. Plot + attribution analysis ----
CSV="$OUT_CSV" OUT="$OUT_PNG" TRIALS="" \
	python benchmark/scripts/analysis/plot_hft_macro_scenarios.py \
	| tee "$REMOTE_ARTIFACTS_DIR/plot_hft_macro_scenarios.log" || true

python benchmark/scripts/analysis/analyze_hft_macro_attribution.py \
	"$OUT_CSV" --out-dir "$RESULTS_DIR" \
	| tee "$REMOTE_ARTIFACTS_DIR/analyze_hft_macro_attribution.log" || true

# ---- 8. Package artifacts ----
echo "ok" > "$RESULTS_DIR/STATUS"
results_leaf="$(basename "$RESULTS_DIR")"
echo "repo/benchmark/results/${results_leaf}" > "$REMOTE_ARTIFACTS_DIR/last_results_dir.txt"
rm -f "$REMOTE_TARBALL"
tar -czf "$REMOTE_TARBALL" -C "$REMOTE_ROOT" \
	"$(basename "$REMOTE_ARTIFACTS_DIR")" \
	"repo/benchmark/results/${results_leaf}"
echo "Remote artifacts ready: $REMOTE_TARBALL"
ENDSSH

remote_ec=$?
if (( remote_ec != 0 )); then
	echo "WARNING: remote pipeline exited with code $remote_ec (artifacts may be partial)" >&2
fi

if ! ssh "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}" "test -f '$REMOTE_TARBALL'"; then
	echo "ERROR: remote tarball missing: ${SSH_USER}@${SERVER_IP}:${REMOTE_TARBALL}" >&2
	exit 1
fi

echo "[Download] Fetching artifacts from ${SSH_USER}@${SERVER_IP} ..."
mkdir -p "$(dirname "$LOCAL_TARBALL")"
scp "${SCP_OPTS[@]}" "${SSH_USER}@${SERVER_IP}:${REMOTE_TARBALL}" "$LOCAL_TARBALL"

LOCAL_STAGING="$LOCAL_OUT_ROOT/.staging/${STAMP}_$$"
rm -rf "$LOCAL_STAGING"
mkdir -p "$LOCAL_STAGING"
tar -xzf "$LOCAL_TARBALL" -C "$LOCAL_STAGING" --no-same-owner --no-same-permissions 2>/dev/null \
	|| tar -xzf "$LOCAL_TARBALL" -C "$LOCAL_STAGING"

results_dir=""
if [[ -f "$LOCAL_STAGING/artifacts/last_results_dir.txt" ]]; then
	results_ref="$(tr -d '\r\n' < "$LOCAL_STAGING/artifacts/last_results_dir.txt")"
	if [[ "$results_ref" = /* ]]; then
		results_dir="$LOCAL_STAGING/repo/benchmark/results/$(basename "$results_ref")"
	elif [[ -d "$LOCAL_STAGING/$results_ref" ]]; then
		results_dir="$LOCAL_STAGING/$results_ref"
	fi
elif compgen -G "$LOCAL_STAGING/repo/benchmark/results/hft_macro_scenarios_tuned_*" >/dev/null; then
	results_dir="$(ls -dt "$LOCAL_STAGING"/repo/benchmark/results/hft_macro_scenarios_tuned_* | head -1)"
fi

flat_dir=""
if [[ -n "$results_dir" && -d "$results_dir" ]]; then
	flat_dir="$LOCAL_RUNS_DIR/$(basename "$results_dir")"
	mkdir -p "$flat_dir"
	cp -a "$results_dir"/. "$flat_dir"/
	cp "$LOCAL_STAGING/artifacts/run_hft_macro_scenarios_tuned.log" "$flat_dir/" 2>/dev/null || true
	cp "$LOCAL_STAGING/artifacts/plot_hft_macro_scenarios.log" "$flat_dir/" 2>/dev/null || true
fi
rm -rf "$LOCAL_STAGING"

echo "Done."
echo "  Tarball   : $LOCAL_TARBALL"
echo "  Runs root : $LOCAL_OUT_ROOT"
if [[ -n "$flat_dir" ]]; then
	echo "  Flat dir  : $flat_dir"
fi
