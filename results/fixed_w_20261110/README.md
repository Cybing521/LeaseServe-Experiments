# Fixed-W sweep: is auth invariant to the budget and the graph? (2026-11-10, corrected)

Prop. 1 says `auth` is a function of `(W(L), V(r,e))` alone. The claim is algebraic, so this sweep
tests it as a measurement: hold `W(L)` fixed, vary the candidate budget `m` and the graph parameters
`(M, efConstruction)`, and classify each miss by the paper's definition.

Setup: one role of the 200k ERBAC ACL (`role=49`, `|V(r,e)|=17,056`), 100 grants drawn
deterministically, every 5th dropped from the log (`hole=5`, 17 dropped, `|W|=17,039`), so
`V(r,e) ⊄ W(L)` and `auth > 0`. No revocations, so no dead nodes. Queries: the first 100 head-role
queries plus one query per dropped document, using the dropped document itself as the query vector.
Read path: graph candidates and exact log candidates merged by distance, filtered against `V(r,e)`,
top-10.

Classification follows the paper, not provenance:

    d not in W                          -> auth
    d in W and in the exact top-m of W  -> ann   (the budget could have reached it)
    d in W but outside that top-m       -> sel   (the budget cut it)

Result over 9 graph configurations (M in {8,16,32} x efC in {50,100,200}) and 4 budgets
(m in {50,150,500,1500}), 36 cells:

| quantity | over the 36 cells |
|---|---|
| auth | **17 in every cell** |
| sel  | **0 in every cell** (no dead nodes here) |
| ann  | 30 -> 0: falls with the budget, and at fixed budget with the graph |
| missing | 47 -> 17 |

At `m=50` the approximation term is 27, 6 and 3 for `(8,100)`, `(16,100)` and `(32,200)`: the graph
quality moves `ann`, as the definition says it should. The budget moves it too (28 -> 0 at
`(8,100)`). Nothing moves `auth`, which stays at the 17 dropped grants.

Earlier revision of this file classified by provenance ("was it a grant?") instead of by membership
in the exact top-m; that mislabelled graph misses as `sel` and is superseded by `fw2/`.

Source: `experiments/cpp/fixed_w_sweep.cpp`. Raw cells: `fw2/fixed_w.csv`.
