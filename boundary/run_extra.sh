#!/bin/bash
# Extra cells: p95 threshold bracket and a 4-core CPU budget.
set -u
cd /root/work/icde
BIN=experiments/boundary/arms_boundary
BASE=data/sift1m/base.f32
Q=data/sift1m/queries_10k.f32
ROLES=acl/uniform1000_1m/roles.bin
OUT=experiments/boundary/results
N=1000000; DIM=128; NK=10; MFAC=15; THREADS=4

mkdir -p $OUT/threshold $OUT/cpu4

threshold() {
  for seed in 0 1 2; do
    for conc in conc90 conc96 conc99; do
      qr=acl/uniform1000_1m/qroles_${conc}_2k.bin
      d=$OUT/threshold/${conc}_s${seed}
      mkdir -p $d
      ARM=D AGGR=50 CHANGE_US=200 TAG=thr_${conc}_s${seed} CONC=$conc \
        CSV=$OUT/matrix.csv \
        $BIN $BASE $Q $ROLES $qr $N $DIM 2000 $NK 100 2000 $THREADS $seed $d $MFAC \
        >> $d/log.txt 2>&1
    done
  done
}

cpu4() {
  for seed in 0 1 2; do
    for arm in D E; do
      merge=0; [ "$arm" = E ] && merge=200
      d=$OUT/cpu4/${arm}_s${seed}
      mkdir -p $d
      taskset -c 0-3 env ARM=$arm AGGR=50 CHANGE_US=200 MERGE_EVERY=$merge \
        TAG=cpu4_${arm}_s${seed} CSV=$OUT/extra.csv \
        $BIN $BASE $Q $ROLES acl/uniform1000_1m/qroles_head10_2k.bin \
        $N $DIM 2000 $NK 10 10000 $THREADS $seed $d $MFAC >> $d/log.txt 2>&1
    done
  done
}

case "${1:-all}" in
  threshold) threshold ;;
  cpu4) cpu4 ;;
  all) threshold; cpu4 ;;
esac
echo "DONE extra $1 $(date -Is)"
