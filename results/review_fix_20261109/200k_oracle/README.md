# 200k ACL, certified arm, oracle cross-check

Server `xeon-host`, 2026-11-09. `CERT=1`, 100 queries, 60 changes, `K=10`, `SELFCHECK=1`.

```
./lease_certified $DATA/base.f32 $DATA/queries.f32 \
  $REPO/cpp/acl/erbac200k/roles.bin \
  $REPO/cpp/acl/erbac200k/qroles_head10.bin \
  200000 128 100 10 10 60 1 0 out 15
```

Kernel: `reads=100 leaks=0 missing=0 auth=0 cert_fallbacks=0 state_mismatch=0`.

Oracle (`oracle.txt`): `reads=100 leaks=0 missing=0 PASS`. The per-role visible set is about
15k documents; the checker also confirms the two dump files (`changes.txt`, `reads.txt`).
