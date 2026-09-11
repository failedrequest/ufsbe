#!/bin/sh
# bench/compare.sh — Compare ufs-fuse v3 (B-ε tree) vs master (indirect ptr)
#
# Usage: sudo sh bench/compare.sh [runtime_secs]
# Default runtime per fio phase is 120 s.  Pass a larger value for longer runs.
#
# For each branch it:
#   1. Checks out the branch and builds
#   2. Creates a fresh newfs -O 2 -U image
#   3. Mounts via FUSE
#   4. Runs a suite of fio micro-benchmarks (seq-write, seq-read, rand-4k-rw,
#      metadata-intensive dir/file create)
#   5. Unmounts and captures results
# Then prints a side-by-side table.
#
# Requirements: git, fio, newfs, mdconfig (run as root)
# The workspace must be a clean git repo with both master and v3 branches.

set -e

RUNTIME=${1:-120}          # seconds per fio job (default 2 min for quick compare)
MOUNT=/mnt/ufs
IMGFILE=/tmp/ufs-bench.img
IMGSIZE=8g
DEV=""
SCRIPT_DIR=$(dirname "$0")
REPO=$(cd "$SCRIPT_DIR/.." && pwd)
RESULTS_DIR="$REPO/bench/results"
mkdir -p "$RESULTS_DIR"

log() { printf '\033[1;36m>>> %s\033[0m\n' "$*" >&2; }
die() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# ── helpers ──────────────────────────────────────────────────────────

ensure_dev() {
    # Create or reuse a vnode-backed md device on IMGFILE
    if [ -f "$IMGFILE" ]; then
        truncate -s 0 "$IMGFILE"
    fi
    truncate -s "$IMGSIZE" "$IMGFILE"
    DEV=$(mdconfig -a -t vnode -f "$IMGFILE")
    DEV="/dev/$DEV"
    log "md device: $DEV"
}

release_dev() {
    local d=${DEV#/dev/}
    mdconfig -d -u "$d" 2>/dev/null || true
    DEV=""
}

mount_fs() {
    local bin=$1
    log "mounting with $bin"
    "$bin" "$DEV" "$MOUNT" -f &
    local pid=$!
    sleep 2
    if ! mount | grep -q "$MOUNT"; then
        die "mount failed (pid $pid)"
    fi
    echo $pid
}

unmount_fs() {
    umount "$MOUNT" 2>/dev/null || umount -f "$MOUNT" 2>/dev/null || true
    sleep 1
}

# ── parse one fio JSON metric ─────────────────────────────────────────

# extract value from fio --output-format=json+
# $1 = json file, $2 = job name, $3 = metric path (e.g. read.bw_mean)
fio_metric() {
    local f=$1 job=$2 metric=$3
    # Use awk to extract — avoids jq dependency
    # Find the job block, then the metric
    python3 -c "
import json, sys
data = json.load(open('$f'))
for j in data.get('jobs', []):
    if j.get('jobname','') == '$job':
        parts = '$metric'.split('.')
        v = j
        for p in parts:
            v = v.get(p, {})
        print(v if isinstance(v, (int,float)) else 0)
        sys.exit(0)
print(0)
" 2>/dev/null || echo 0
}

# ── run one full benchmark suite for a built binary ──────────────────

run_suite() {
    local label=$1 binary=$2 out=$3

    log "--- $label ---"

    # Fresh filesystem
    newfs -O 2 -U "$DEV" > /dev/null 2>&1

    mkdir -p "$MOUNT"
    local fpid
    fpid=$(mount_fs "$binary")

    # fio suite — one JSON output file
    # Run fio; strip any stderr error lines that fio prepends before the JSON
    local rawtmp
    rawtmp=$(mktemp /tmp/fio-raw-XXXXXX.json)
    fio \
        --output-format=json+ \
        --output="$rawtmp" \
        --time_based=1 \
        --runtime="$RUNTIME" \
        --ioengine=sync \
        --direct=0 \
        --buffered=1 \
        --fallocate=none \
        --directory="$MOUNT" \
        --name=seqwr  --rw=write    --bs=128k  --size=64m  --numjobs=4 \
        --name=seqrd  --rw=read     --bs=128k  --size=64m  --numjobs=4 \
        --name=rand4k --rw=randrw   --rwmixread=60 --bs=4k --size=32m  --numjobs=6 \
        2>/dev/null || true
    # Strip leading non-JSON lines (fio error messages)
    local firstjson
    firstjson=$(grep -n '^{' "$rawtmp" | head -1 | cut -d: -f1)
    if [ -n "$firstjson" ]; then
        tail -n "+$firstjson" "$rawtmp" > "$out"
    else
        cp "$rawtmp" "$out"
    fi
    rm -f "$rawtmp"

    unmount_fs
    kill "$fpid" 2>/dev/null || true
}

# ── metadata benchmark (create/stat/unlink) ───────────────────────────

run_meta() {
    local label=$1 binary=$2

    log "--- $label metadata ---"
    newfs -O 2 -U "$DEV" > /dev/null 2>&1
    mkdir -p "$MOUNT"
    local fpid
    fpid=$(mount_fs "$binary")

    local t0 t1 elapsed
    t0=$(date +%s%3N)
    local n=0
    while [ $n -lt 10000 ]; do
        echo x > "$MOUNT/f$n"
        n=$((n+1))
    done
    t1=$(date +%s%3N)
    local create_ms=$((t1 - t0))

    t0=$(date +%s%3N)
    n=0
    while [ $n -lt 10000 ]; do
        stat "$MOUNT/f$n" > /dev/null 2>&1
        n=$((n+1))
    done
    t1=$(date +%s%3N)
    local stat_ms=$((t1 - t0))

    t0=$(date +%s%3N)
    n=0
    while [ $n -lt 10000 ]; do
        rm "$MOUNT/f$n"
        n=$((n+1))
    done
    t1=$(date +%s%3N)
    local unlink_ms=$((t1 - t0))

    unmount_fs
    kill "$fpid" 2>/dev/null || true

    echo "${create_ms} ${stat_ms} ${unlink_ms}"
}

# ── build a branch ────────────────────────────────────────────────────

build_branch() {
    local branch=$1 outbin=$2
    log "building branch $branch"
    git -C "$REPO" stash 2>/dev/null || true
    git -C "$REPO" checkout "$branch"
    make -C "$REPO" clean > /dev/null 2>&1
    make -C "$REPO" > /dev/null 2>&1
    cp "$REPO/ufs-fuse" "$outbin"
}

# ── main ──────────────────────────────────────────────────────────────

[ "$(id -u)" -eq 0 ] || die "must run as root"
which fio  > /dev/null 2>&1 || die "fio not found (pkg install fio)"
kldstat | grep -q fusefs || kldload fusefs

ORIG_BRANCH=$(git -C "$REPO" rev-parse --abbrev-ref HEAD)

BIN_MASTER="/tmp/ufs-fuse-master"
BIN_V3="/tmp/ufs-fuse-v3"

# Build both branches
build_branch master "$BIN_MASTER"
build_branch v3     "$BIN_V3"

# Restore original branch
git -C "$REPO" checkout "$ORIG_BRANCH" > /dev/null 2>&1

# Create device once, reuse across runs (newfs between each)
ensure_dev

OUT_MASTER="$RESULTS_DIR/master-fio.json"
OUT_V3="$RESULTS_DIR/v3-fio.json"

# ── fio suites ────────────────────────────────────────────────────────
run_suite "master (indirect-ptr)" "$BIN_MASTER" "$OUT_MASTER"
run_suite "v3 (B-ε tree)"         "$BIN_V3"     "$OUT_V3"

# ── metadata suites ───────────────────────────────────────────────────
log "running metadata benchmark — master"
META_MASTER=$(run_meta "master" "$BIN_MASTER")
log "running metadata benchmark — v3"
META_V3=$(run_meta "v3" "$BIN_V3")

release_dev

# ── extract metrics ───────────────────────────────────────────────────

get_bw()    { fio_metric "$1" "$2" "${3}.bw_mean"; }    # KB/s
get_iops()  { fio_metric "$1" "$2" "${3}.iops_mean"; }
get_lat()   { fio_metric "$1" "$2" "${3}.lat_ns.mean"; } # ns

# ── print report ──────────────────────────────────────────────────────

fmt_bw() {
    # KB/s → MiB/s
    python3 -c "print('%.1f MiB/s' % ($1 / 1024))" 2>/dev/null || echo "${1} KB/s"
}

fmt_iops() {
    python3 -c "print('%.0f IOPS' % $1)" 2>/dev/null || echo "${1} IOPS"
}

fmt_lat() {
    python3 -c "print('%.1f µs' % ($1 / 1000))" 2>/dev/null || echo "${1} ns"
}

printf '\n'
printf '%-40s  %20s  %20s\n' 'Benchmark' 'master (indir-ptr)' 'v3 (B-ε tree)'
printf '%s\n' "$(printf -- '-%.0s' $(seq 1 84))"

for job in seqwr seqrd rand4k; do
    case $job in
        seqwr)
            label="Seq write 128k (4 jobs)"
            bw_m=$(get_bw   "$OUT_MASTER" "$job" "write")
            bw_v=$(get_bw   "$OUT_V3"     "$job" "write")
            printf '%-40s  %20s  %20s\n' "$label (bw)"   "$(fmt_bw  $bw_m)" "$(fmt_bw  $bw_v)"
            ;;
        seqrd)
            label="Seq read 128k (4 jobs)"
            bw_m=$(get_bw   "$OUT_MASTER" "$job" "read")
            bw_v=$(get_bw   "$OUT_V3"     "$job" "read")
            printf '%-40s  %20s  %20s\n' "$label (bw)"   "$(fmt_bw  $bw_m)" "$(fmt_bw  $bw_v)"
            ;;
        rand4k)
            label="Rand 4k rw 60/40 (8 jobs)"
            rm_m=$(get_iops "$OUT_MASTER" "$job" "read")
            rm_v=$(get_iops "$OUT_V3"     "$job" "read")
            wm_m=$(get_iops "$OUT_MASTER" "$job" "write")
            wm_v=$(get_iops "$OUT_V3"     "$job" "write")
            rl_m=$(get_lat  "$OUT_MASTER" "$job" "read")
            rl_v=$(get_lat  "$OUT_V3"     "$job" "read")
            printf '%-40s  %20s  %20s\n' "$label (read IOPS)"  "$(fmt_iops $rm_m)" "$(fmt_iops $rm_v)"
            printf '%-40s  %20s  %20s\n' "$label (write IOPS)" "$(fmt_iops $wm_m)" "$(fmt_iops $wm_v)"
            printf '%-40s  %20s  %20s\n' "$label (read lat)"   "$(fmt_lat  $rl_m)" "$(fmt_lat  $rl_v)"
            ;;
    esac
done

printf '%s\n' "$(printf -- '-%.0s' $(seq 1 84))"
printf '%-40s  %20s  %20s\n' 'Benchmark' 'master (indir-ptr)' 'v3 (B-ε tree)'
printf '%s\n' "$(printf -- '-%.0s' $(seq 1 84))"

# Metadata
mcr_m=$(echo $META_MASTER | awk '{print $1}')
mcr_v=$(echo $META_V3     | awk '{print $1}')
mst_m=$(echo $META_MASTER | awk '{print $2}')
mst_v=$(echo $META_V3     | awk '{print $2}')
mul_m=$(echo $META_MASTER | awk '{print $3}')
mul_v=$(echo $META_V3     | awk '{print $3}')

rate_m() { python3 -c "print('%.0f files/s' % (10000*1000/$1))" 2>/dev/null || echo "?"; }

printf '%-40s  %20s  %20s\n' 'Metadata: 10k creates'  "$(rate_m $mcr_m)" "$(rate_m $mcr_v)"
printf '%-40s  %20s  %20s\n' 'Metadata: 10k stats'    "$(rate_m $mst_m)" "$(rate_m $mst_v)"
printf '%-40s  %20s  %20s\n' 'Metadata: 10k unlinks'  "$(rate_m $mul_m)" "$(rate_m $mul_v)"

printf '%s\n' "$(printf -- '-%.0s' $(seq 1 84))"
printf '\nResults JSON: %s  %s\n' "$OUT_MASTER" "$OUT_V3"
printf 'Branch restored to: %s\n\n' "$ORIG_BRANCH"
