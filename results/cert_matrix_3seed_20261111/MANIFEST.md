# Table III raw matrix (3 seeds)

1M ERBAC, 200 changes, 4 readers + 1 writer, 200 reads, head-10, `nq=200`, 3 seeds.
Build: `g++ -O3 -march=native -std=c++17 -I experiments/cpp/third_party experiments/cpp/lease_certified.cpp -o lease_snap -pthread`.

| arm | predicate | lost grant | Table III row |
|---|---|---|---|
| `full_drop0` | cert + log | no | cert.+log, no loss |
| `nocert_drop0` | graph+log, no check | no | graph+log, no check |
| `versioneq_drop0` | version equality | no | version equality |
| `nolog_drop0` | cert, no log | no | cert., no log |
| `full_drop5` | cert + log | 1/5 | cert.+log, lost |
| `nocert_drop5` | graph+log, no check | 1/5 | graph+log, no check, lost |

The six arm means/sds round to the values in Table III: 0.45±0.02, 0.44±0.04, 22.37±0.42,
21.67±1.24, 21.76±1.24, 0.47±0.04. Fallbacks: 200/200 for version equality and no-log;
178--187/200 for full_drop5. This directory is the raw record, not a replacement run.
