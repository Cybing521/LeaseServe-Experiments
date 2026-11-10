# Top-1 counterexample with the pin == a control (2026-11-10)

Command (cwd = this directory's build dir):

    e1_auth_miss $DATA/base.f32 $DATA/queries.f32 \
      $REPO/acl/erbac200k/roles.bin $REPO/acl/erbac200k/qroles_head10.bin \
      200000 128 200 10 10 e1h 15

`e1h/` is the run quoted in the paper (`nq` read from the qroles file: 100,
constructed 93). `e1ctl/` is the same binary on the 100-query all-role file
(constructed 29) and is kept only as a cross-check.

| arm | auth miss | fallback | p50 (ms) |
|---|---|---|---|
| no certificate, no log | 93 | 0 | 0.219 |
| unsound certificate | 93 | 0 | 0.120 |
| certificate + log | 0 | 0 | 0.096 |
| version equality (pin = 1) | 0 | 93 | 2.895 |
| version equality (pin = a = 0) | 0 | 0 | 0.184 |

`pin_eq_a reads=93 exact_topk=93 short=0`.

The last row is a control added on 2026-11-10: the same version-equality rule,
read at pin == a, takes the graph branch and returns the exact top-k on all 93
constructed queries. It fails above a only because the build epoch is behind the
pin, not because the predicate is wrong. The paper's 2.874 ms / 93 fallback row
is the pre-control run of the same binary; `summary_prev.txt` keeps it.
