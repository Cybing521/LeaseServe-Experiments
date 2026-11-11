#!/bin/bash
# Boundary / mechanism-ablation run matrix.  Sequential on purpose: latency.
set -u
cd /root/work/icde
BIN=experiments/boundary/arms_boundary
BASE=data/sift1m/base.f32
Q=data/sift1m/queries_10k.f32
ROLES=acl/uniform1000_1m/roles.bin
QR_HEAD=acl/uniform1000_1m/qroles_head10_2k.bin
QR_ZIPF=acl/uniform1000_1m/qroles_zipf_2k.bin
QR_UNI=acl/uniform1000_1m/qroles_uniform_2k.bin
OUT=experiments/boundary/results
N=1000000; DIM=128; KNN=10; MFAC=15; THREADS=4; CHANGES=2000

mkdir -p $OUT/ablation $OUT/matrix $OUT/longrun

run_ablation() {
  local arm=$1 aggr=$2 seed=$3 merge=$4 drop=$5
  local d=$OUT/ablation/${arm}_a${aggr}_m${merge}_d${drop}_s${seed}
  mkdir -p $d
  ARM=$arm AGGR=$aggr CHANGE_US=200 DROP_EVERY=$drop MERGE_EVERY=$merge \
    TAG=abl_${arm}_a${aggr}_m${merge}_d${drop}_s${seed} \
    CSV=$OUT/ablation.csv \
    $BIN $BASE $Q $ROLES $QR_HEAD $N $DIM 2000 $KNN 10 $CHANGES $THREADS $seed $d $MFAC \
    >> $d/log.txt 2>&1
}

run_matrix() {
  local conc=$1 qr=$2 K=$3 aggr=$4 seed=$5
  local d=$OUT/matrix/${conc}_K${K}_a${aggr}_s${seed}
  mkdir -p $d
  ARM=D AGGR=$aggr CHANGE_US=200 \
    TAG=mat_${conc}_K${K}_a${aggr}_s${seed} CONC=$conc \
    CSV=$OUT/matrix.csv \
    $BIN $BASE $Q $ROLES $qr $N $DIM 2000 $KNN $K $CHANGES $THREADS $seed $d $MFAC \
    >> $d/log.txt 2>&1
}

run_exact() {
  local conc=$1 qr=$2 seed=$3
  local d=$OUT/matrix/exact_${conc}_s${seed}
  mkdir -p $d
  ARM=X AGGR=50 TAG=exact_${conc}_s${seed} CONC=$conc CSV=$OUT/matrix.csv \
    $BIN $BASE $Q $ROLES $qr $N $DIM 2000 $KNN 0 0 $THREADS $seed $d $MFAC \
    >> $d/log.txt 2>&1
}

run_long() {
  local arm=$1 seed=$2 merge=$3
  local d=$OUT/longrun/${arm}_m${merge}_s${seed}
  mkdir -p $d
  ARM=$arm AGGR=50 CHANGE_US=200 MERGE_EVERY=$merge \
    TAG=long_${arm}_m${merge}_s${seed} CSV=$OUT/longrun.csv TRACE=$d/trace.csv RSS_TRACE=$d/rss.csv \
    $BIN $BASE $Q $ROLES $QR_HEAD $N $DIM 2000 $KNN 10 10000 $THREADS $seed $d $MFAC \
    >> $d/log.txt 2>&1
}

case "${1:-all}" in
  ablation)
    for seed in 0 1 2; do
      for aggr in 0 50 100; do
        for arm in A B C D E; do
          merge=0; [ "$arm" = E ] && merge=100
          run_ablation $arm $aggr $seed $merge 0
        done
      done
      run_ablation C 50 $seed 0 5
      run_ablation D 50 $seed 0 5
    done
    ;;
  matrix)
    for seed in 0 1 2; do
      for K in 10 50 100; do
        for aggr in 0 50 100; do
          run_matrix head10 $QR_HEAD $K $aggr $seed
          run_matrix zipf   $QR_ZIPF $K $aggr $seed
          run_matrix uniform $QR_UNI  $K $aggr $seed
        done
      done
      run_exact head10 $QR_HEAD $seed
      run_exact zipf   $QR_ZIPF $seed
      run_exact uniform $QR_UNI  $seed
    done
    ;;
  longrun)
    for seed in 0 1 2; do
      run_long D $seed 0
      run_long E $seed 200
      run_long E $seed 500
    done
    ;;
esac
echo "DONE $1 $(date -Is)"
