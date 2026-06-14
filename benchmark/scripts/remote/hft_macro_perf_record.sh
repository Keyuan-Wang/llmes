#!/usr/bin/env bash
set -euo pipefail

# Remote hft_macro perf record runner with Linux measurement hygiene.
#
# Usage:
#   SERVER_IP=1.2.3.4 REPO_URL=https://github.com/you/llmes.git \
#     bash benchmark/scripts/remote/hft_macro_perf_record.sh

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
REMOTE_TARBALL="${REMOTE_TARBALL:-$REMOTE_ROOT/hft_macro_perf_record_artifacts.tgz}"

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$(cd "$SCRIPTS_DIR/../.." && pwd)"
LOCAL_OUT_ROOT="${LOCAL_OUT_ROOT:-$ROOT_DIR/server_results}"
LOCAL_OUT_ROOT="$(mkdir -p "$LOCAL_OUT_ROOT" && cd "$LOCAL_OUT_ROOT" && pwd)"
LOCAL_ARCHIVES_DIR="$LOCAL_OUT_ROOT/archives"
LOCAL_RUNS_DIR="$LOCAL_OUT_ROOT/hft_macro/perf_record"
mkdir -p "$LOCAL_ARCHIVES_DIR" "$LOCAL_RUNS_DIR"

# Forwarded to local/hft_macro_perf_record.sh
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-1000000}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ITERS="${ITERS:-40}"
SEED="${SEED:-42}"
EVENTS="${EVENTS:-cycles,branch-misses}"
FREQ="${FREQ:-8000}"
CALL_GRAPH="${CALL_GRAPH:-dwarf}"
USE_CHRT_FIFO="${USE_CHRT_FIFO:-1}"
ENABLE_LTO="${ENABLE_LTO:-0}"
BUILD_DIR="${BUILD_DIR:-}"
VERSION_TAG="${VERSION_TAG:-perf_record}"
ENABLE_LINUX_ISOLATION="${ENABLE_LINUX_ISOLATION:-1}"
INSTALL_DEPS="${INSTALL_DEPS:-1}"
SYNC_LOCAL_SCRIPTS="${SYNC_LOCAL_SCRIPTS:-1}"
REMOTE_SYNC_TARBALL="${REMOTE_SYNC_TARBALL:-$REMOTE_ROOT/bench_perf_scripts_sync.tgz}"

if [[ -z "$SERVER_IP" || -z "$REPO_URL" ]]; then
	echo "Usage: SERVER_IP=1.2.3.4 REPO_URL=git@github.com:you/llmes.git $0" >&2
	exit 1
fi

SSH_OPTS=(-i "$SSH_KEY" -p "$SSH_PORT" -o StrictHostKeyChecking=accept-new)
SCP_OPTS=(-i "$SSH_KEY" -P "$SSH_PORT" -o StrictHostKeyChecking=accept-new)
STAMP="$(date +%Y%m%d_%H%M%S)"
TAG_SAFE="$(echo "${COMMIT_SHA:-$BRANCH}" | tr '/:' '__')"
LOCAL_TARBALL="$LOCAL_ARCHIVES_DIR/hft_macro_perf_record_${TAG_SAFE}_${STAMP}.tgz"

echo "===== Remote hft_macro perf record pipeline ====="
echo "  Server     : ${SSH_USER}@${SERVER_IP}"
echo "  Checkout   : ${COMMIT_SHA:-$BRANCH}"
echo "  Isolation  : $ENABLE_LINUX_ISOLATION"
echo "  Events     : $EVENTS @ ${FREQ}Hz, call-graph=$CALL_GRAPH, chrt=$USE_CHRT_FIFO"
echo "  LTO        : $ENABLE_LTO"
echo ""

if [[ "$SYNC_LOCAL_SCRIPTS" == "1" ]]; then
	LOCAL_SYNC_TAR="$(mktemp /tmp/bench_perf_scripts_XXXXXX.tgz)"
	tar czf "$LOCAL_SYNC_TAR" -C "$ROOT_DIR" \
		benchmark/scripts/lib/bench_linux_isolation.sh \
		benchmark/scripts/local/hft_macro_perf_record.sh
	ssh "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}" "mkdir -p '$REMOTE_ROOT'"
	scp "${SCP_OPTS[@]}" "$LOCAL_SYNC_TAR" "${SSH_USER}@${SERVER_IP}:${REMOTE_SYNC_TARBALL}"
	rm -f "$LOCAL_SYNC_TAR"
	echo "  Synced local perf-record scripts to remote"
fi

ssh "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}" \
	"REPO_URL='$REPO_URL' BRANCH='$BRANCH' COMMIT_SHA='$COMMIT_SHA' \
	REMOTE_ROOT='$REMOTE_ROOT' REMOTE_REPO_DIR='$REMOTE_REPO_DIR' \
	REMOTE_ARTIFACTS_DIR='$REMOTE_ARTIFACTS_DIR' REMOTE_TARBALL='$REMOTE_TARBALL' \
	ORDERS='$ORDERS' LEVELS='$LEVELS' BATCH_SIZE='$BATCH_SIZE' \
	WARMUP_ITERS='$WARMUP_ITERS' ITERS='$ITERS' SEED='$SEED' \
	EVENTS='$EVENTS' FREQ='$FREQ' CALL_GRAPH='$CALL_GRAPH' USE_CHRT_FIFO='$USE_CHRT_FIFO' \
	ENABLE_LTO='$ENABLE_LTO' BUILD_DIR='$BUILD_DIR' VERSION_TAG='$VERSION_TAG' \
	ENABLE_LINUX_ISOLATION='$ENABLE_LINUX_ISOLATION' INSTALL_DEPS='$INSTALL_DEPS' \
	REMOTE_SYNC_TARBALL='$REMOTE_SYNC_TARBALL' bash -s" <<'ENDSSH'
set -euo pipefail

mkdir -p "$REMOTE_ROOT" "$REMOTE_ARTIFACTS_DIR"

if [[ "$INSTALL_DEPS" == "1" ]]; then
	export DEBIAN_FRONTEND=noninteractive
	apt-get update
	apt-get install -y --no-install-recommends \
		git ca-certificates build-essential cmake python3 python3-venv python3-pip \
		numactl util-linux
	apt-get install -y --no-install-recommends linux-tools-common linux-tools-generic \
		"linux-tools-$(uname -r)" linux-perf 2>/dev/null || \
		apt-get install -y --no-install-recommends linux-tools-common linux-perf || true
fi

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
		echo "ERROR: unknown ref: $COMMIT_SHA" >&2
		exit 1
	fi
	CHECKOUT_DESC="commit $(git rev-parse HEAD) (detached)"
else
	git checkout "$BRANCH"
	git reset --hard "origin/$BRANCH"
	CHECKOUT_DESC="branch $BRANCH @ $(git rev-parse HEAD)"
fi

if [[ -f "$REMOTE_SYNC_TARBALL" ]]; then
	echo "--- Applying synced perf-record scripts ---"
	tar -xzf "$REMOTE_SYNC_TARBALL" -C "$REMOTE_REPO_DIR"
	rm -f "$REMOTE_SYNC_TARBALL"
fi

# perf sampling typically needs paranoid <= 2 on cloud VMs.
if [[ -w /proc/sys/kernel/perf_event_paranoid ]]; then
	echo 1 > /proc/sys/kernel/perf_event_paranoid
fi

OUT_DIR="$REMOTE_REPO_DIR/benchmark/results/hft_macro_perf_record_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"

env \
	ORDERS="$ORDERS" LEVELS="$LEVELS" BATCH_SIZE="$BATCH_SIZE" \
	WARMUP_ITERS="$WARMUP_ITERS" ITERS="$ITERS" SEED="$SEED" \
	EVENTS="$EVENTS" FREQ="$FREQ" CALL_GRAPH="$CALL_GRAPH" USE_CHRT_FIFO="$USE_CHRT_FIFO" \
	ENABLE_LTO="$ENABLE_LTO" BUILD_DIR="$BUILD_DIR" VERSION_TAG="$VERSION_TAG" \
	COMMIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" \
	OUT_DIR="$OUT_DIR" ENABLE_LINUX_ISOLATION="$ENABLE_LINUX_ISOLATION" \
	bash benchmark/scripts/local/hft_macro_perf_record.sh \
	| tee "$REMOTE_ARTIFACTS_DIR/run_hft_macro_perf_record.log"

{
	echo "timestamp=$(date -Iseconds)"
	echo "checkout=$CHECKOUT_DESC"
	echo "commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
	echo "orders=$ORDERS"
	echo "levels=$LEVELS"
	echo "batch_size=$BATCH_SIZE"
	echo "warmup_iters=$WARMUP_ITERS"
	echo "iters=$ITERS"
	echo "seed=$SEED"
	echo "events=$EVENTS"
	echo "freq=$FREQ"
	echo "call_graph=$CALL_GRAPH"
	echo "use_chrt_fifo=$USE_CHRT_FIFO"
	echo "enable_lto=$ENABLE_LTO"
	echo "linux_isolation=$ENABLE_LINUX_ISOLATION"
	echo "results_dir=$OUT_DIR"
	echo
	uname -a || true
	echo
	lscpu || true
	echo
	numactl --hardware 2>/dev/null || true
	echo
	perf --version 2>/dev/null || true
} > "$OUT_DIR/env.txt"
cp "$OUT_DIR/env.txt" "$REMOTE_ARTIFACTS_DIR/env.txt"
echo "ok" > "$OUT_DIR/STATUS"

results_leaf="$(basename "$OUT_DIR")"
echo "repo/benchmark/results/${results_leaf}" > "$REMOTE_ARTIFACTS_DIR/last_results_dir.txt"
rm -f "$REMOTE_TARBALL"
tar -czf "$REMOTE_TARBALL" -C "$REMOTE_ROOT" \
	"$(basename "$REMOTE_ARTIFACTS_DIR")" \
	"repo/benchmark/results/${results_leaf}"

echo "Remote artifacts ready: $REMOTE_TARBALL"
ENDSSH

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
elif compgen -G "$LOCAL_STAGING/repo/benchmark/results/hft_macro_perf_record_*" >/dev/null; then
	results_dir="$(ls -dt "$LOCAL_STAGING"/repo/benchmark/results/hft_macro_perf_record_* | head -1)"
fi

flat_dir=""
if [[ -n "$results_dir" && -d "$results_dir" ]]; then
	flat_dir="$LOCAL_RUNS_DIR/$(basename "$results_dir")"
	mkdir -p "$flat_dir"
	cp -a "$results_dir"/. "$flat_dir"/
	cp "$LOCAL_STAGING/artifacts/run_hft_macro_perf_record.log" "$flat_dir/" 2>/dev/null || true
fi
rm -rf "$LOCAL_STAGING"

echo "Done."
echo "  Tarball   : $LOCAL_TARBALL"
echo "  Runs root : $LOCAL_OUT_ROOT"
if [[ -n "$flat_dir" ]]; then
	echo "  Flat dir  : $flat_dir"
	echo "  report    : $flat_dir/report.txt"
fi
