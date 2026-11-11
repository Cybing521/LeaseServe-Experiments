# 1M `nocert_drop5`, post-fix re-run and oracle

Server `llm_01` (16-core cgroup quota), 2026-11-09 22:16. The point is not latency (the build
took 255 s on a busy host) but that the repaired accounting leaves the 1M row unchanged and that
the independent oracle reproduces it.

```
CERT=0 DROP_GRANT=5 CHANGE_US=500 MERGE_EVERY=0 WARMUP=100 SELFCHECK=1 \
CHANGES_OUT=changes.txt QUERIES_OUT=reads.txt \
./lease_certified /root/autodl-tmp/icde_cm/base.f32 /root/autodl-tmp/icde_cm/queries.f32 \
  /root/autodl-tmp/icde_cm/acl/erbac100/roles.bin \
  /root/autodl-tmp/icde_cm/acl/erbac100/qroles_head10.bin \
  1000000 128 200 10 10 200 4 0 out1m 15
```

Kernel `summary.txt`:

```
reads=200 p50=0.563026 p95=2.16974 p99=3.73782 leaks=0 missing=3 auth=0
cert_fallbacks=0 state_mismatch=0 visible_total=772232 cur_total=772232
```

The recorded pre-fix run (`cert_matrix_20261108/nocert_drop5`) has the same `missing=3 auth=0`;
the accounting fix cannot change it because that run never merges (`a_L=0`), so the old
`grant_v <= a_L` test and graph membership agree.

Independent oracle (`oracle.txt`):

```
python3 oracle_check.py --base .../base.f32 --queries .../queries.f32 \
  --roles .../acl/erbac100/roles.bin --n 1000000 --dim 128 --k 10 \
  --changes changes.txt --reads reads.txt
reads=200 leaks=0 missing=3   PASS
```

The oracle orders by `(distance, id)`, the same pair order as the kernel's `exact_topk`, so a tied
k-th distance is resolved uniquely; this run has no tie warning and the aggregate still matches 3.
