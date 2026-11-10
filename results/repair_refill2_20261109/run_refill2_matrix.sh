#!/bin/bash
# ROE+refill A/B: same protocol as run_repair_matrix.sh, REFILL=1, separate results dir.
cd $REPO
BIN=./experiments/cpp/arms_repair
BASE=data/sift1m/base.f32
Q=data/sift1m/queries.f32
ROLES=acl/uniform1000_1m/roles.bin
QR=acl/uniform1000_1m/qroles.bin
OUT=results/repair_refill2_20261109
run() {
  local tag=$1 aggr=$2 ch=$3 mf=$4 sd=$5
  local d=$OUT/$tag; mkdir -p $d
  printf "AGGR=%s BURST=0 n=1000000 dim=128 nq=200 k=10 K=10 changes=%s threads=4 seed=%s mfac=%s REFILL=1\n" $aggr $ch $sd $mf > $d/cmd.txt
  REFILL=1 AGGR=$aggr BURST=0 $BIN $BASE $Q $ROLES $QR 1000000 128 200 10 10 $ch 4 $sd $d $mf >> $d/cmd.txt 2>&1
}
for mf in 1 3 15; do
  for ch in 10000 20000 30000; do
    for sd in 0 1 2; do run rev_phi$((ch/500))_m${mf}_s$sd 0 $ch $mf $sd; done
  done
done
for mf in 1 15; do
  for ch in 5000 10000; do
    for sd in 0 1; do run gr_$((ch/500))_m${mf}_s$sd 100 $ch $mf $sd; done
  done
done
echo REFILL_DONE
