# Top-1 counterexample: returned length check (2026-11-09)

The paper's 93/100 table counts `auth_miss` (the granted document is the exact
top-1 of the authorized set and is not returned). It does not support the old
sentence "each query returns nine results"; this rerun measures the returned
length directly.

Data: 200k ERBAC (`recovery_20261109/acl/erbac200k`), 200 queries, ten promoted
roles, k=10, mfac=15. The run constructed 180 queries whose global nearest
neighbour was outside the role; `auth_miss=180` without the log and 0 with it.
Every arm returned exactly 10 results (`min_len=10`, `max_len=10`,
`returned=1800` for 180 queries), so the failure is a missing top-1, not a short
list.

Command:

```bash
e1_auth_miss base200k.f32 queries500.f32 \
  acl/erbac200k/roles.bin acl/erbac200k/qroles_head10.bin \
  200000 128 200 10 10 out/ 15
```

`summary.txt` is the raw output. The paper's 93/100 sample is a different ACL
draw; the returned-length conclusion is measured here on the same harness.
