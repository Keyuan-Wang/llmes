#!/usr/bin/env bash
set -euo pipefail

# Remote HFT per-scenario runner with Linux measurement hygiene.
# Key features: dynamic clean-CPU selection, NUMA/task binding, optional
# performance governor, device IRQ movement away from the benchmark CPU, and
# before/after kernel activity snapshots.

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

TRIALS="${TRIALS:-10}"
ITERS="${ITERS:-1}"
WARMUP_ITERS="${WARMUP_ITERS:-1}"
ORDERS="${ORDERS:-100000}"
LEVELS="${LEVELS:-100}"
BATCH_SIZE="${BATCH_SIZE:-100000}"
SEED="${SEED:-42}"
FOCUS="${FOCUS:-all}"
VERSION_TAG="${VERSION_TAG:-tuned}"

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
echo ""

ssh "${SSH_OPTS[@]}" "${SSH_USER}@${SERVER_IP}" \
	"REPO_URL='$REPO_URL' BRANCH='$BRANCH' COMMIT_SHA='$COMMIT_SHA' \
	REMOTE_ROOT='$REMOTE_ROOT' REMOTE_REPO_DIR='$REMOTE_REPO_DIR' \
	REMOTE_ARTIFACTS_DIR='$REMOTE_ARTIFACTS_DIR' REMOTE_TARBALL='$REMOTE_TARBALL' \
	TRIALS='$TRIALS' ITERS='$ITERS' WARMUP_ITERS='$WARMUP_ITERS' ORDERS='$ORDERS' LEVELS='$LEVELS' \
	BATCH_SIZE='$BATCH_SIZE' SEED='$SEED' FOCUS='$FOCUS' VERSION_TAG='$VERSION_TAG' \
	BENCH_CPU='$BENCH_CPU' NUMA_NODE='$NUMA_NODE' CPU_PROBE_SECONDS='$CPU_PROBE_SECONDS' \
	USE_NUMACTL='$USE_NUMACTL' SET_PERFORMANCE_GOVERNOR='$SET_PERFORMANCE_GOVERNOR' \
	RESTORE_GOVERNOR='$RESTORE_GOVERNOR' REDUCE_BACKGROUND_NOISE='$REDUCE_BACKGROUND_NOISE' \
	STOP_NOISY_TIMERS='$STOP_NOISY_TIMERS' AVOID_IRQ_ON_BENCH_CPU='$AVOID_IRQ_ON_BENCH_CPU' \
	AVOID_IRQ_ON_SMT_SIBLINGS='$AVOID_IRQ_ON_SMT_SIBLINGS' RESTORE_IRQ_AFFINITY='$RESTORE_IRQ_AFFINITY' \
	AGGRESSIVE_ISOLATION='$AGGRESSIVE_ISOLATION' PIN_WORKQUEUES_AWAY='$PIN_WORKQUEUES_AWAY' \
	STOP_IRQBALANCE='$STOP_IRQBALANCE' DISABLE_KERNEL_WATCHDOGS='$DISABLE_KERNEL_WATCHDOGS' \
	LOCK_CPU_DMA_LATENCY='$LOCK_CPU_DMA_LATENCY' USE_CHRT_FIFO='$USE_CHRT_FIFO' \
	REALTIME_PRIORITY='$REALTIME_PRIORITY' INSTALL_DEPS='$INSTALL_DEPS' bash -s" <<'ENDSSH'
set -euo pipefail

mkdir -p "$REMOTE_ROOT" "$REMOTE_ARTIFACTS_DIR"

if [[ "$INSTALL_DEPS" == "1" ]]; then
	export DEBIAN_FRONTEND=noninteractive
	apt-get update
	apt-get install -y --no-install-recommends \
		git ca-certificates build-essential cmake python3 python3-venv python3-pip \
		numactl util-linux
	apt-get install -y --no-install-recommends linux-tools-common || true
fi

if [[ ! -d "$REMOTE_REPO_DIR/.git" ]]; then
	git clone "$REPO_URL" "$REMOTE_REPO_DIR"
fi

cd "$REMOTE_REPO_DIR"
git fetch --all --prune --tags
if [[ -n "$COMMIT_SHA" ]]; then
	git checkout --detach "$COMMIT_SHA"
	CHECKOUT_DESC="commit $(git rev-parse HEAD) (detached)"
else
	git checkout "$BRANCH"
	git pull --ff-only origin "$BRANCH"
	CHECKOUT_DESC="branch $BRANCH @ $(git rev-parse HEAD)"
fi

python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLMES_BUILD_BENCHMARKS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure | tee "$REMOTE_ARTIFACTS_DIR/ctest.log"

RESULTS_DIR="$REMOTE_REPO_DIR/benchmark/results/hft_macro_scenarios_tuned_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR"
OUT_CSV="$RESULTS_DIR/hft_macro_scenario_cycles.csv"
OUT_PNG="$RESULTS_DIR/hft_macro_scenario_cycles_distributions.png"
IRQ_MOVED_LOG="$RESULTS_DIR/irq_affinity_moved.txt"

write_governors() {
	local out="$1"
	: > "$out"
	for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		[[ -e "$f" ]] || continue
		printf "%s=%s\n" "$f" "$(cat "$f")" >> "$out"
	done
}

set_governor() {
	[[ "$SET_PERFORMANCE_GOVERNOR" == "1" ]] || return 0
	write_governors "$REMOTE_ARTIFACTS_DIR/governors_before.txt"
	for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		[[ -e "$f" ]] || continue
		echo performance > "$f" 2>/dev/null || true
	done
	write_governors "$REMOTE_ARTIFACTS_DIR/governors_during.txt"
}

restore_governor() {
	[[ "$RESTORE_GOVERNOR" == "1" ]] || return 0
	local snap="$REMOTE_ARTIFACTS_DIR/governors_before.txt"
	[[ -f "$snap" ]] || return 0
	while IFS='=' read -r f gov; do
		[[ -w "$f" ]] || continue
		echo "$gov" > "$f" 2>/dev/null || true
	done < "$snap"
	write_governors "$REMOTE_ARTIFACTS_DIR/governors_after.txt"
}

reduce_noise() {
	[[ "$REDUCE_BACKGROUND_NOISE" == "1" ]] || return 0
	{
		uptime || true
		ps -eo pid,psr,pcpu,pmem,comm --sort=-pcpu | head -25 || true
		systemctl list-timers --all 2>/dev/null || true
	} > "$REMOTE_ARTIFACTS_DIR/noise_before.txt"
	if [[ "$STOP_NOISY_TIMERS" == "1" ]] && command -v systemctl >/dev/null 2>&1; then
		for unit in apt-daily.timer apt-daily-upgrade.timer apt-daily.service \
			apt-daily-upgrade.service man-db.timer man-db.service \
			plocate-updatedb.timer plocate-updatedb.service updatedb.timer \
		updatedb.service fstrim.timer fstrim.service; do
			systemctl stop "$unit" 2>/dev/null || true
		done
		if [[ "$STOP_IRQBALANCE" == "1" ]]; then
			systemctl stop irqbalance.service 2>/dev/null || true
		fi
	fi
	{
		uptime || true
		ps -eo pid,psr,pcpu,pmem,comm --sort=-pcpu | head -25 || true
		systemctl list-timers --all 2>/dev/null || true
	} > "$REMOTE_ARTIFACTS_DIR/noise_after.txt"
}

apply_aggressive_isolation() {
	[[ "$AGGRESSIVE_ISOLATION" == "1" ]] || return 0
	local log="$RESULTS_DIR/aggressive_isolation.txt"
	{
		echo "aggressive_isolation=1"
		echo "bench_cpu=$BENCH_CPU_SELECTED"
		echo "smt_siblings=$SMT_SIBLINGS"
		echo "avoid_irq_on_smt_siblings=$AVOID_IRQ_ON_SMT_SIBLINGS"
		echo "pin_workqueues_away=$PIN_WORKQUEUES_AWAY"
		echo "stop_irqbalance=$STOP_IRQBALANCE"
		echo "disable_kernel_watchdogs=$DISABLE_KERNEL_WATCHDOGS"
		echo "lock_cpu_dma_latency=$LOCK_CPU_DMA_LATENCY"
		echo "use_chrt_fifo=$USE_CHRT_FIFO"
		echo "realtime_priority=$REALTIME_PRIORITY"
	} > "$log"

	if [[ "$DISABLE_KERNEL_WATCHDOGS" == "1" ]]; then
		{
			echo "--- watchdog/sysctl writes ---"
			sysctl -w kernel.nmi_watchdog=0 2>&1 || true
			sysctl -w kernel.watchdog=0 2>&1 || true
			sysctl -w kernel.timer_migration=1 2>&1 || true
			sysctl -w kernel.sched_rt_runtime_us=-1 2>&1 || true
		} >> "$log"
	fi

	if [[ "$LOCK_CPU_DMA_LATENCY" == "1" && -w /dev/cpu_dma_latency ]]; then
		{
			echo "--- cpu_dma_latency ---"
		} >> "$log"
		(
			exec 9>/dev/cpu_dma_latency
			printf '\0\0\0\0' >&9
			echo "locked_to_0us=1" >> "$log"
			sleep 86400
		) &
		DMA_LATENCY_PID=$!
		echo "dma_latency_pid=$DMA_LATENCY_PID" >> "$log"
	fi

	if [[ "$PIN_WORKQUEUES_AWAY" == "1" ]]; then
		python3 - "$BENCH_CPU_SELECTED" "$SMT_SIBLINGS" "$AVOID_IRQ_ON_SMT_SIBLINGS" "$RESULTS_DIR/workqueue_cpumask.txt" <<'PY'
import sys
from pathlib import Path

bench_cpu = int(sys.argv[1])
siblings_text = sys.argv[2]
avoid_siblings = sys.argv[3] == "1"
out = Path(sys.argv[4])

def expand(text):
	cpus = set()
	for part in text.split(","):
		part = part.strip()
		if "-" in part:
			a, b = part.split("-", 1)
			if a.isdigit() and b.isdigit():
				cpus.update(range(int(a), int(b) + 1))
		elif part.isdigit():
			cpus.add(int(part))
	return cpus

def read(path):
	try:
		return Path(path).read_text(errors="ignore").strip()
	except OSError:
		return ""

def mask(cpus):
	value = 0
	for cpu in cpus:
		value |= 1 << cpu
	return format(value, "x")

online = expand(read("/sys/devices/system/cpu/online")) or {bench_cpu}
avoid = {bench_cpu}
if avoid_siblings:
	avoid |= expand(siblings_text)
target = online - avoid
if not target:
	target = online - {bench_cpu}
target_mask = mask(target)
rows = [f"online={','.join(map(str, sorted(online)))}",
        f"avoid={','.join(map(str, sorted(avoid)))}",
        f"target={','.join(map(str, sorted(target)))}",
        f"target_mask={target_mask}"]

paths = [Path("/sys/devices/virtual/workqueue/cpumask")]
paths += sorted(Path("/sys/devices/virtual/workqueue").glob("*/cpumask"))
for path in paths:
	before = read(path)
	status = "skipped"
	err = ""
	try:
		path.write_text(target_mask + "\n")
		status = "moved"
	except OSError as exc:
		err = str(exc)
	after = read(path)
	rows.append(f"{path}\t{status}\tbefore={before}\tafter={after}\terror={err}")
out.write_text("\n".join(rows) + "\n")
PY
		cp "$RESULTS_DIR/workqueue_cpumask.txt" "$REMOTE_ARTIFACTS_DIR/workqueue_cpumask.txt" 2>/dev/null || true
	fi
	cp "$log" "$REMOTE_ARTIFACTS_DIR/aggressive_isolation.txt" 2>/dev/null || true
}

snapshot_kernel() {
	local phase="$1"
	local dir="$2"
	mkdir -p "$dir"
	cat /proc/interrupts > "$dir/interrupts_${phase}.txt" 2>/dev/null || true
	cat /proc/softirqs > "$dir/softirqs_${phase}.txt" 2>/dev/null || true
	cat /proc/schedstat > "$dir/schedstat_${phase}.txt" 2>/dev/null || true
	python3 - "$dir/interrupts_${phase}.txt" "$dir/irq_affinity_${phase}.txt" <<'PY'
import re, sys
from pathlib import Path

def read(path):
	try:
		return Path(path).read_text(errors="ignore").strip()
	except OSError:
		return ""

rows = []
for line in Path(sys.argv[1]).read_text(errors="ignore").splitlines():
	if ":" not in line:
		continue
	name, rest = line.split(":", 1)
	irq = name.strip()
	if not re.fullmatch(r"\d+", irq):
		continue
	base = Path("/proc/irq") / irq
	rows.append((
		irq,
		read(base / "smp_affinity_list"),
		read(base / "smp_affinity"),
		read(base / "effective_affinity_list"),
		read(base / "effective_affinity"),
		" ".join([irq] + rest.split()),
	))

with Path(sys.argv[2]).open("w") as f:
	f.write("irq\tsmp_affinity_list\tsmp_affinity\teffective_affinity_list\teffective_affinity\tlabel\n")
	for row in rows:
		f.write("\t".join(row) + "\n")
PY
}

select_cpu() {
	local out="$1"
	local selected="$2"
	if [[ "$BENCH_CPU" != "auto" ]]; then
		echo "$BENCH_CPU" > "$selected"
		{
			echo "mode=manual"
			echo "selected_cpu=$BENCH_CPU"
		} > "$out"
		return
	fi

	local probe="$RESULTS_DIR/cpu_probe"
	mkdir -p "$probe"
	snapshot_kernel probe_before "$probe"
	sleep "$CPU_PROBE_SECONDS"
	snapshot_kernel probe_after "$probe"

	python3 - "$probe" "$out" "$selected" "$CPU_PROBE_SECONDS" <<'PY'
import re, sys
from pathlib import Path

probe = Path(sys.argv[1])
report = Path(sys.argv[2])
selected_file = Path(sys.argv[3])
seconds = float(sys.argv[4])

def expand(text):
	cpus = set()
	for part in text.split(","):
		part = part.strip()
		if not part:
			continue
		if "-" in part:
			a, b = part.split("-", 1)
			if a.isdigit() and b.isdigit():
				cpus.update(range(int(a), int(b) + 1))
		elif part.isdigit():
			cpus.add(int(part))
	return cpus

def read(path):
	try:
		return Path(path).read_text(errors="ignore").strip()
	except OSError:
		return ""

def cpus_online():
	online = expand(read("/sys/devices/system/cpu/online"))
	return sorted(online) if online else [0]

def siblings(cpu):
	return expand(read(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")) or {cpu}

def parse_irq_affinity(path):
	res = {}
	if not path.exists():
		return res
	for line in path.read_text(errors="ignore").splitlines()[1:]:
		parts = line.split("\t")
		if len(parts) >= 6:
			res[parts[0]] = {
				"smp": parts[1],
				"effective": parts[3],
				"label": parts[5],
			}
	return res

def parse_interrupts(path):
	lines = path.read_text(errors="ignore").splitlines()
	ncpu = 0
	rows = {}
	for line in lines:
		if line.lstrip().startswith("CPU0"):
			ncpu = len(re.findall(r"\bCPU\d+\b", line))
			continue
		if ":" not in line or not ncpu:
			continue
		name, rest = line.split(":", 1)
		parts = rest.split()
		if len(parts) < ncpu:
			continue
		try:
			vals = [int(x) for x in parts[:ncpu]]
		except ValueError:
			continue
		rows[name.strip()] = (vals, " ".join([name.strip()] + parts[ncpu:]))
	return ncpu, rows

def parse_softirqs(path):
	lines = path.read_text(errors="ignore").splitlines()
	ncpu = 0
	rows = {}
	for line in lines:
		if line.lstrip().startswith("CPU0"):
			ncpu = len(re.findall(r"\bCPU\d+\b", line))
			continue
		if ":" not in line or not ncpu:
			continue
		name, rest = line.split(":", 1)
		parts = rest.split()
		if len(parts) < ncpu:
			continue
		try:
			rows[name.strip()] = [int(x) for x in parts[:ncpu]]
		except ValueError:
			pass
	return ncpu, rows

def delta_rows(before, after, ncpu, tuple_rows):
	labels = set(before) | set(after)
	out = {}
	for label in labels:
		if tuple_rows:
			b = before.get(label, ([0] * ncpu, ""))[0]
			a = after.get(label, ([0] * ncpu, ""))[0]
		else:
			b = before.get(label, [0] * ncpu)
			a = after.get(label, [0] * ncpu)
		if len(b) < ncpu:
			b = b + [0] * (ncpu - len(b))
		if len(a) < ncpu:
			a = a + [0] * (ncpu - len(a))
		out[label] = [a[i] - b[i] for i in range(ncpu)]
	return out

online = cpus_online()
aff = parse_irq_affinity(probe / "irq_affinity_probe_after.txt")
ncpu_i, irq_b = parse_interrupts(probe / "interrupts_probe_before.txt")
_, irq_a = parse_interrupts(probe / "interrupts_probe_after.txt")
ncpu_s, soft_b = parse_softirqs(probe / "softirqs_probe_before.txt")
_, soft_a = parse_softirqs(probe / "softirqs_probe_after.txt")
ncpu = max(ncpu_i, ncpu_s, max(online) + 1)
irq_d = delta_rows(irq_b, irq_a, ncpu, True)
soft_d = delta_rows(soft_b, soft_a, ncpu, False)

device_re = re.compile(r"(PCI|MSI|MSIX|virtio|ahci|xhci|nvme|ena|eth|ens|enp)", re.I)
scores = {}
details = {}
for cpu in online:
	sib = siblings(cpu)
	loc = irq_d.get("LOC", [0] * ncpu)[cpu] if cpu < ncpu else 0
	movable = unmovable = 0
	for irq, d in irq_d.items():
		if not irq.isdigit() or cpu >= len(d) or d[cpu] <= 0:
			continue
		info = aff.get(irq, {})
		label = info.get("label", irq_b.get(irq, ([], irq))[1])
		if not device_re.search(label):
			continue
		allowed = expand(info.get("smp", ""))
		if cpu in allowed and any(c != cpu for c in allowed):
			movable += d[cpu]
		else:
			unmovable += d[cpu]
	soft = sum(soft_d.get(k, [0] * ncpu)[cpu] for k in ("RCU", "TIMER", "SCHED") if cpu < ncpu)
	net_block = sum(soft_d.get(k, [0] * ncpu)[cpu] for k in ("NET_RX", "BLOCK") if cpu < ncpu)
	sibling_unmovable = 0
	for scpu in sib:
		if scpu == cpu or scpu >= ncpu:
			continue
		for irq, d in irq_d.items():
			if not irq.isdigit() or d[scpu] <= 0:
				continue
			info = aff.get(irq, {})
			label = info.get("label", irq_b.get(irq, ([], irq))[1])
			if device_re.search(label) and expand(info.get("smp", "")) == {scpu}:
				sibling_unmovable += d[scpu]
	score = (
		unmovable * 1000
		+ movable * 20
		+ net_block * 5
		+ soft * 2
		+ loc * 0.5
		+ sibling_unmovable * 200
		+ (50000 if 0 in sib else 0)
	)
	scores[cpu] = score
	details[cpu] = (sib, loc, movable, unmovable, soft, net_block, sibling_unmovable)

selected = min(scores, key=lambda c: (scores[c], c))
selected_file.write_text(str(selected) + "\n")

with report.open("w") as f:
	f.write(f"probe_seconds={seconds}\n")
	f.write(f"selected_cpu={selected}\n")
	f.write("score = unmovable_irq*1000 + movable_irq*20 + net_block*5 + rcu_timer_sched*2 + loc*0.5 + sibling_unmovable_irq*200 + cpu0_sibling_penalty\n")
	for cpu in online:
		sib, loc, movable, unmovable, soft, net_block, sibling_unmovable = details[cpu]
		f.write(
			f"cpu={cpu} score={scores[cpu]:.2f} siblings={','.join(map(str, sorted(sib)))} "
			f"loc={loc} movable_irq={movable} unmovable_irq={unmovable} "
			f"rcu_timer_sched={soft} net_block={net_block} sibling_unmovable_irq={sibling_unmovable}\n"
		)
PY
}

detect_numa_node() {
	local cpu="$1"
	if [[ "$NUMA_NODE" != "auto" ]]; then
		echo "$NUMA_NODE"
		return
	fi
	local node
	node="$(lscpu -p=CPU,NODE 2>/dev/null | awk -F, -v c="$cpu" '$1 == c {print $2; exit}')"
	[[ -n "$node" && "$node" != "-" ]] && echo "$node" || echo 0
}

detect_siblings() {
	local cpu="$1"
	local f="/sys/devices/system/cpu/cpu${cpu}/topology/thread_siblings_list"
	[[ -r "$f" ]] && cat "$f" || echo "$cpu"
}

move_irqs() {
	local snapshot="$1"
	local out="$2"
	[[ "$AVOID_IRQ_ON_BENCH_CPU" == "1" ]] || return 0
	python3 - "$snapshot" "$out" "$BENCH_CPU_SELECTED" "$SMT_SIBLINGS" "$AVOID_IRQ_ON_SMT_SIBLINGS" <<'PY'
import re, sys
from pathlib import Path

snapshot, out, bench_cpu = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])
siblings_text, avoid_siblings = sys.argv[4], sys.argv[5] == "1"
device_re = re.compile(r"(PCI|MSI|MSIX|virtio|ahci|xhci|nvme|ena|eth|ens|enp)", re.I)

def expand(text):
	cpus = set()
	for part in text.split(","):
		part = part.strip()
		if "-" in part:
			a, b = part.split("-", 1)
			if a.isdigit() and b.isdigit():
				cpus.update(range(int(a), int(b) + 1))
		elif part.isdigit():
			cpus.add(int(part))
	return cpus

def fmt(cpus):
	return ",".join(str(c) for c in sorted(cpus))

def mask(cpus):
	m = 0
	for c in cpus:
		m |= 1 << c
	return format(m, "x")

def read(path):
	try:
		return Path(path).read_text(errors="ignore").strip()
	except OSError:
		return ""

online = expand(read("/sys/devices/system/cpu/online")) or {bench_cpu}
avoid = {bench_cpu}
if avoid_siblings:
	avoid |= expand(siblings_text)
target = online - avoid
if not target:
	target = online - {bench_cpu}
target_list, target_mask = fmt(target), mask(target)

with out.open("w") as f:
	f.write("avoid_cpus\t" + fmt(avoid) + "\n")
	f.write("target_cpus\t" + target_list + "\n")
	f.write("irq\tstatus\toriginal_smp_affinity_list\ttarget_smp_affinity_list\tafter_smp_affinity_list\tafter_effective_affinity_list\tlabel\terror\n")
	for line in snapshot.read_text(errors="ignore").splitlines()[1:]:
		parts = line.split("\t")
		if len(parts) < 6:
			continue
		irq, smp_list, _smp_mask, effective_list, _effective_mask, label = parts[:6]
		if not re.fullmatch(r"\d+", irq) or not device_re.search(label):
			continue
		if avoid.isdisjoint(expand(smp_list)) and avoid.isdisjoint(expand(effective_list)):
			f.write(f"{irq}\tskipped_not_on_avoid_cpu\t{smp_list}\t{target_list}\t{smp_list}\t{effective_list}\t{label}\t\n")
			continue
		if not target:
			f.write(f"{irq}\tskipped_no_target\t{smp_list}\t\t{smp_list}\t{effective_list}\t{label}\t\n")
			continue
		base = Path("/proc/irq") / irq
		err = ""
		status = "moved"
		try:
			(base / "smp_affinity_list").write_text(target_list + "\n")
		except OSError as exc:
			try:
				(base / "smp_affinity").write_text(target_mask + "\n")
				status = "moved_mask"
			except OSError as exc2:
				status = "failed"
				err = f"list:{exc};mask:{exc2}".replace("\t", " ")
		f.write(
			f"{irq}\t{status}\t{smp_list}\t{target_list}\t"
			f"{read(base / 'smp_affinity_list')}\t{read(base / 'effective_affinity_list')}\t{label}\t{err}\n"
		)
PY
	cp "$out" "$REMOTE_ARTIFACTS_DIR/irq_affinity_moved.txt" 2>/dev/null || true
}

restore_irqs() {
	[[ "$RESTORE_IRQ_AFFINITY" == "1" && -f "$IRQ_MOVED_LOG" ]] || return 0
	python3 - "$IRQ_MOVED_LOG" "$REMOTE_ARTIFACTS_DIR/irq_affinity_restore.log" <<'PY'
import sys
from pathlib import Path

for_out = []
for line in Path(sys.argv[1]).read_text(errors="ignore").splitlines():
	if line.startswith("avoid_cpus") or line.startswith("target_cpus"):
		continue
	parts = line.split("\t")
	if len(parts) < 3 or parts[0] == "irq":
		continue
	irq, status, original = parts[:3]
	if not status.startswith("moved") or not original:
		continue
	try:
		(Path("/proc/irq") / irq / "smp_affinity_list").write_text(original + "\n")
		after = (Path("/proc/irq") / irq / "smp_affinity_list").read_text(errors="ignore").strip()
		for_out.append(f"{irq}\trestored\t{after}\t")
	except OSError as exc:
		for_out.append(f"{irq}\tfailed\t\t{str(exc).replace(chr(9), ' ')}")
Path(sys.argv[2]).write_text("irq\tstatus\trestored_smp_affinity_list\terror\n" + "\n".join(for_out) + "\n")
PY
	cp "$REMOTE_ARTIFACTS_DIR/irq_affinity_restore.log" "$RESULTS_DIR/irq_affinity_restore.log" 2>/dev/null || true
}

DMA_LATENCY_PID=""

summarize_kernel_delta() {
	local out="$1"
	python3 - "$RESULTS_DIR" "$out" "$BENCH_CPU_SELECTED" <<'PY'
import re, sys
from pathlib import Path

root, out, bench_cpu = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3])

def ncpu(line):
	return len(re.findall(r"\bCPU\d+\b", line))

def parse_irq(path):
	n, rows = 0, {}
	if not path.exists():
		return n, rows
	for line in path.read_text(errors="ignore").splitlines():
		if line.lstrip().startswith("CPU0"):
			n = ncpu(line)
			continue
		if ":" not in line or not n:
			continue
		name, rest = line.split(":", 1)
		parts = rest.split()
		if len(parts) < n:
			continue
		try:
			vals = [int(x) for x in parts[:n]]
		except ValueError:
			continue
		rows[name.strip()] = (vals, " ".join([name.strip()] + parts[n:]))
	return n, rows

def parse_soft(path):
	n, rows = 0, {}
	if not path.exists():
		return n, rows
	for line in path.read_text(errors="ignore").splitlines():
		if line.lstrip().startswith("CPU0"):
			n = ncpu(line)
			continue
		if ":" not in line or not n:
			continue
		name, rest = line.split(":", 1)
		parts = rest.split()
		if len(parts) < n:
			continue
		try:
			rows[name.strip()] = [int(x) for x in parts[:n]]
		except ValueError:
			pass
	return n, rows

def affinity():
	p = root / "irq_affinity_after.txt"
	res = {}
	if p.exists():
		for line in p.read_text(errors="ignore").splitlines()[1:]:
			parts = line.split("\t")
			if len(parts) >= 6:
				res[parts[0]] = parts
	return res

def write_delta(title, before, after, fh, aff=None, *, tuple_rows=False):
	labels = set(before) | set(after)
	lengths = []
	for row_map in (before, after):
		for value in row_map.values():
			if tuple_rows:
				lengths.append(len(value[0]))
			else:
				lengths.append(len(value))
	n = max(lengths or [0])
	tot = [0] * n
	entries = []
	for label in labels:
		if tuple_rows:
			b = before.get(label, ([0] * n, ""))[0]
			a = after.get(label, ([0] * n, ""))[0]
		else:
			b = before.get(label, [0] * n)
			a = after.get(label, [0] * n)
		if len(b) < n:
			b = b + [0] * (n - len(b))
		if len(a) < n:
			a = a + [0] * (n - len(a))
		d = [a[i] - b[i] for i in range(n)]
		for i, v in enumerate(d):
			tot[i] += v
		entries.append((d[bench_cpu] if bench_cpu < n else 0, sum(d), label, d))
	fh.write(f"===== {title} delta =====\n")
	fh.write(f"bench_cpu={bench_cpu}\n")
	fh.write("total_by_cpu=" + ",".join(map(str, tot)) + "\n")
	if bench_cpu < n:
		fh.write(f"bench_cpu_total={tot[bench_cpu]}\n")
	for bd, td, label, d in sorted(entries, reverse=True)[:20]:
		if bd or td:
			fh.write(f"  bench_cpu_delta={bd} total_delta={td} label={label} per_cpu={d}\n")
	if aff:
		fh.write("irq_affinity_for_top_interrupt_rows:\n")
		for bd, td, label, _d in sorted(entries, reverse=True)[:20]:
			irq = label.split(maxsplit=1)[0]
			if irq in aff:
				row = aff[irq]
				fh.write(f"  irq={irq} bench_cpu_delta={bd} smp_affinity_list={row[1]} effective_affinity_list={row[3]} label={row[5]}\n")
	fh.write("\n")

_, ib = parse_irq(root / "interrupts_before.txt")
_, ia = parse_irq(root / "interrupts_after.txt")
_, sb = parse_soft(root / "softirqs_before.txt")
_, sa = parse_soft(root / "softirqs_after.txt")
with out.open("w") as fh:
	write_delta("interrupts", ib, ia, fh, affinity(), tuple_rows=True)
	write_delta("softirqs", sb, sa, fh)
PY
}

package_artifacts() {
	[[ -n "${RESULTS_DIR:-}" ]] || return 0
	local results_leaf
	results_leaf="$(basename "$RESULTS_DIR")"
	echo "repo/benchmark/results/${results_leaf}" > "$REMOTE_ARTIFACTS_DIR/last_results_dir.txt"
	rm -f "$REMOTE_TARBALL"
	if [[ -d "$RESULTS_DIR" ]]; then
		tar -czf "$REMOTE_TARBALL" -C "$REMOTE_ROOT" \
			"$(basename "$REMOTE_ARTIFACTS_DIR")" \
			"repo/benchmark/results/${results_leaf}"
	else
		tar -czf "$REMOTE_TARBALL" -C "$REMOTE_ROOT" \
			"$(basename "$REMOTE_ARTIFACTS_DIR")"
	fi
	echo "Remote artifacts ready: $REMOTE_TARBALL"
}

finalize_run() {
	local exit_code="${1:-0}"
	if [[ -n "${RESULTS_DIR:-}" && -f "$OUT_CSV" ]]; then
		if [[ ! -f "$OUT_PNG" ]]; then
			echo "--- Plot per-scenario distributions (finalize) ---"
			CSV="$OUT_CSV" OUT="$OUT_PNG" TRIALS="" \
				python benchmark/scripts/analysis/plot_hft_macro_scenarios.py \
				| tee "$REMOTE_ARTIFACTS_DIR/plot_hft_macro_scenarios.log" || true
		fi
		if [[ ! -f "$RESULTS_DIR/env.txt" ]]; then
			{
				echo "timestamp=$(date -Iseconds)"
				echo "checkout=${CHECKOUT_DESC:-unknown}"
				echo "commit=$(git -C "$REMOTE_REPO_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"
				echo "trials=$TRIALS"
				echo "seed=$SEED"
				echo "results_dir=$RESULTS_DIR"
				echo "finalize_exit_code=$exit_code"
			} > "$RESULTS_DIR/env.txt"
			cp "$RESULTS_DIR/env.txt" "$REMOTE_ARTIFACTS_DIR/env.txt" 2>/dev/null || true
		fi
		if [[ "$exit_code" -eq 0 ]]; then
			echo "ok" > "$RESULTS_DIR/STATUS"
		else
			echo "partial exit_code=$exit_code" > "$RESULTS_DIR/STATUS"
		fi
	fi
	package_artifacts || true
}

cleanup() {
	restore_irqs
	restore_governor
	if [[ -n "${DMA_LATENCY_PID:-}" ]]; then
		kill "$DMA_LATENCY_PID" 2>/dev/null || true
		wait "$DMA_LATENCY_PID" 2>/dev/null || true
	fi
}

trap 'ec=$?; finalize_run "$ec" || true; cleanup || true' EXIT

set_governor
reduce_noise

select_cpu "$RESULTS_DIR/cpu_selection_report.txt" "$RESULTS_DIR/selected_cpu"
BENCH_CPU_SELECTED="$(cat "$RESULTS_DIR/selected_cpu")"
NUMA_NODE_SELECTED="$(detect_numa_node "$BENCH_CPU_SELECTED")"
SMT_SIBLINGS="$(detect_siblings "$BENCH_CPU_SELECTED")"

apply_aggressive_isolation
snapshot_kernel pre_irq_tuning "$RESULTS_DIR"
move_irqs "$RESULTS_DIR/irq_affinity_pre_irq_tuning.txt" "$IRQ_MOVED_LOG"
snapshot_kernel before "$RESULTS_DIR"

if [[ "$USE_NUMACTL" == "1" ]] && command -v numactl >/dev/null 2>&1; then
	RUN_PREFIX=(numactl --physcpubind="$BENCH_CPU_SELECTED" --membind="$NUMA_NODE_SELECTED")
else
	RUN_PREFIX=(taskset -c "$BENCH_CPU_SELECTED")
fi
if [[ "$USE_CHRT_FIFO" == "1" ]] && command -v chrt >/dev/null 2>&1; then
	RUN_PREFIX=(chrt -f "$REALTIME_PRIORITY" "${RUN_PREFIX[@]}")
fi

env \
	TRIALS="$TRIALS" ITERS="$ITERS" WARMUP_ITERS="$WARMUP_ITERS" \
	ORDERS="$ORDERS" LEVELS="$LEVELS" BATCH_SIZE="$BATCH_SIZE" \
	SEED="$SEED" FOCUS="$FOCUS" VERSION_TAG="$VERSION_TAG" \
	COMMIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" OUT_DIR="$RESULTS_DIR" OUT_CSV="$OUT_CSV" \
	"${RUN_PREFIX[@]}" bash benchmark/scripts/local/hft_macro_scenarios.sh \
	| tee "$REMOTE_ARTIFACTS_DIR/run_hft_macro_scenarios_tuned.log"

echo "[$(date -Iseconds)] benchmark finished, capturing post-run snapshots ..."
snapshot_kernel after "$RESULTS_DIR"
summarize_kernel_delta "$RESULTS_DIR/kernel_activity_delta.txt" || true
cp "$RESULTS_DIR/kernel_activity_delta.txt" "$REMOTE_ARTIFACTS_DIR/kernel_activity_delta.txt" 2>/dev/null || true
restore_irqs
snapshot_kernel restored "$RESULTS_DIR"

CSV="$OUT_CSV" OUT="$OUT_PNG" TRIALS="" \
	python benchmark/scripts/analysis/plot_hft_macro_scenarios.py \
	| tee "$REMOTE_ARTIFACTS_DIR/plot_hft_macro_scenarios.log" || true

python benchmark/scripts/analysis/analyze_hft_macro_attribution.py \
	"$OUT_CSV" --out-dir "$RESULTS_DIR" \
	| tee "$REMOTE_ARTIFACTS_DIR/analyze_hft_macro_attribution.log" || true

echo "[$(date -Iseconds)] writing env.txt and packaging ..."
{
	echo "timestamp=$(date -Iseconds)"
	echo "checkout=${CHECKOUT_DESC}"
	echo "commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
	echo "trials=$TRIALS"
	echo "focus=$FOCUS"
	echo "seed=$SEED"
	echo "orders=$ORDERS"
	echo "levels=$LEVELS"
	echo "batch_size=$BATCH_SIZE"
	echo "iters=$ITERS"
	echo "warmup_iters=$WARMUP_ITERS"
	echo "version_tag=$VERSION_TAG"
	echo
	echo "===== tuning ====="
	echo "bench_cpu=$BENCH_CPU_SELECTED"
	echo "numa_node=$NUMA_NODE_SELECTED"
	echo "smt_siblings=$SMT_SIBLINGS"
	echo "cpu_probe_seconds=$CPU_PROBE_SECONDS"
	echo "use_numactl=$USE_NUMACTL"
	echo "set_performance_governor=$SET_PERFORMANCE_GOVERNOR"
	echo "restore_governor=$RESTORE_GOVERNOR"
	echo "reduce_background_noise=$REDUCE_BACKGROUND_NOISE"
	echo "avoid_irq_on_bench_cpu=$AVOID_IRQ_ON_BENCH_CPU"
	echo "avoid_irq_on_smt_siblings=$AVOID_IRQ_ON_SMT_SIBLINGS"
	echo "restore_irq_affinity=$RESTORE_IRQ_AFFINITY"
	echo "aggressive_isolation=$AGGRESSIVE_ISOLATION"
	echo "pin_workqueues_away=$PIN_WORKQUEUES_AWAY"
	echo "stop_irqbalance=$STOP_IRQBALANCE"
	echo "disable_kernel_watchdogs=$DISABLE_KERNEL_WATCHDOGS"
	echo "lock_cpu_dma_latency=$LOCK_CPU_DMA_LATENCY"
	echo "use_chrt_fifo=$USE_CHRT_FIFO"
	echo "realtime_priority=$REALTIME_PRIORITY"
	echo "run_prefix=${RUN_PREFIX[*]}"
	echo "cpu_selection_report=$RESULTS_DIR/cpu_selection_report.txt"
	echo "kernel_activity_delta=$RESULTS_DIR/kernel_activity_delta.txt"
	echo "irq_affinity_moved=$IRQ_MOVED_LOG"
	echo "aggressive_isolation_log=$RESULTS_DIR/aggressive_isolation.txt"
	echo "workqueue_cpumask=$RESULTS_DIR/workqueue_cpumask.txt"
	echo
	uname -a || true
	echo
	lscpu || true
	echo
	numactl --hardware 2>/dev/null || true
	echo
	g++ --version || true
	echo
	cmake --version || true
} > "$RESULTS_DIR/env.txt"
cp "$RESULTS_DIR/env.txt" "$REMOTE_ARTIFACTS_DIR/env.txt"
echo "ok" > "$RESULTS_DIR/STATUS"
ENDSSH

remote_ec=$?
if (( remote_ec != 0 )); then
	echo "WARNING: remote pipeline exited with code $remote_ec (artifacts may still be partial)" >&2
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
	elif [[ -d "$results_ref" ]]; then
		results_dir="$results_ref"
	fi
elif compgen -G "$LOCAL_STAGING/repo/benchmark/results/hft_macro_scenarios_tuned_*" >/dev/null; then
	results_dir="$(ls -dt "$LOCAL_STAGING"/repo/benchmark/results/hft_macro_scenarios_tuned_* | head -1)"
fi

flat_dir=""
if [[ -n "$results_dir" && -d "$results_dir" ]]; then
	flat_dir="$LOCAL_RUNS_DIR/$(basename "$results_dir")"
	mkdir -p "$flat_dir"
	cp -a "$results_dir"/. "$flat_dir"/
	if [[ -f "$LOCAL_STAGING/artifacts/run_hft_macro_scenarios_tuned.log" ]]; then
		cp "$LOCAL_STAGING/artifacts/run_hft_macro_scenarios_tuned.log" "$flat_dir/" 2>/dev/null || true
	fi
	if [[ -f "$LOCAL_STAGING/artifacts/plot_hft_macro_scenarios.log" ]]; then
		cp "$LOCAL_STAGING/artifacts/plot_hft_macro_scenarios.log" "$flat_dir/" 2>/dev/null || true
	fi
fi
rm -rf "$LOCAL_STAGING"

echo "Done."
echo "  Tarball   : $LOCAL_TARBALL"
echo "  Runs root : $LOCAL_OUT_ROOT"
if [[ -n "$flat_dir" ]]; then
	echo "  Flat dir  : $flat_dir"
fi
