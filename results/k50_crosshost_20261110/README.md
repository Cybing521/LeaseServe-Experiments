# K=50 cross-host check (2026-11-10)

The supplement's K sweep reports K=50 as p50 0.378 ms (range 0.37–3.45) over three runs, 75% of
reads leased. Re-running the same configuration on the second host (Ryzen 9950X) with the current
kernel gives:

    certified reads=200 certified_reads=150 read_s=0.310822 p50=0.203532 p95=2.88602
    split lease_reads=150 lease_p50=0.189967 lease_p95=0.250721 exact_reads=50

So the leased share reproduces exactly (150/200), but the p50 does not (0.204 against 0.378), and
neither does p95 (2.89 against 6.50). The K=50 point mixes 150 lease reads with 50 exact scans, so
its median sits on the boundary between two populations and moves with which side the middle
samples land on. That is why the paper quotes the range and relies on K=100 for the
full-coverage claim.

Command:

    MEM_BALLAST_MB=3140 MMAX=5000 CHANGE_US=500 WARMUP=100 \
    ./lc base.f32 queries_10k.f32 acl/erbac100_1m/roles.bin acl/erbac100_1m/qroles_all.bin \
       1000000 128 200 10 50 100 4 0 k50r1 15
