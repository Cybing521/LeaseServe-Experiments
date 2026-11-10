#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BASE=${BASE:-/path/to/sift1m/base.f32}
BIN=${BIN:-$ROOT/lease_certified}
OUT=${OUT:-$ROOT/results/official10k_repro}
D=$ROOT/results/ab_stream_official10k_20261109
mkdir -p "$OUT"
CHANGE_US=500 DELTA_CAP=4096 MERGE_EVERY=0 DROP_GRANT=0 CERT=1 WRITERS=1 \
MMAX=5000 WARMUP=100 \
"$BIN" "$BASE" "$D/queries10k_real.f32" \
  "$ROOT/cpp/acl/erbac200k/roles.bin" "$ROOT/cpp/acl/erbac200k/qroles_10000.bin" \
  200000 128 10000 10 10 100 4 0 "$OUT" 15
