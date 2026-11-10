# Fixed-W sweep: is auth invariant to the budget and the graph? (2026-11-10)

Prop. 1 says `auth` is a function of `(W(L), V(r,e))` alone. The claim is algebraic, so this sweep
tests whether it holds as a measurement: hold `W(L)` fixed, vary the candidate budget `m` and the
graph parameters `(M, efConstruction)`, and report the axis split.

Setup: one role of the 200k ERBAC ACL (`role=49`, `|V(r,e)|=17,056`), 100 grants drawn
deterministically, every 5th dropped from the log (`hole=5`, 17 dropped, `|W|=17,039`), so
`V(r,e) ⊄ W(L)` and `auth > 0`. Queries: the first 100 head-role queries plus one query per dropped
document, using the dropped document itself as the query vector, so the hole is actually exercised.
Read path: graph candidates and exact log candidates merged by distance, filtered against `V(r,e)`,
top-10.

    ./fixed_w_sweep base.f32 queries_10k.f32 acl/erbac200k/roles.bin qroles_head10.bin \
        200000 128 100 10 1 5 100 0 fw

Result over 9 graph configurations (M in {8,16,32} x efC in {50,100,200}) and 4 budgets
(m in {50,150,500,1500}), 36 cells:

| | auth | ann | sel | missing |
|---|---|---|---|---|
| range over all 36 cells | **17 in every cell** | **0 in every cell** | 0--30 | 17--47 |

`auth` never moves: it is the 17 dropped grants, whatever the budget and whatever the graph.
`sel` moves with the budget (28 at m=50, 0 at m=500 for M=8) and, at a fixed budget, with the graph
(28 / 6 / 3 at m=50 for (8,100) / (16,100) / (32,200)). `ann` is zero because the log contributes
exact candidates; the budget only decides how many of the role's own documents the graph can supply.

Latency grows with the budget (1.25 ms at m=50 to 2.15 ms at m=1500), and the log merge is part of
it: every query scores all 100 granted documents.

Source: `experiments/cpp/fixed_w_sweep.cpp`. Raw cells: `fw/fixed_w.csv`.
