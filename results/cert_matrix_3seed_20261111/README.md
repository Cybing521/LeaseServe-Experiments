# Certificate matrix, 1M ERBAC, three seeds

Source: 1M SIFT base, `acl/erbac100` ACL, `qroles_head10`, 200 changes, 4 readers + 1 writer,
200 reads, snapshot `lease_certified_c5ef787`. Each row is mean±sd over seeds 0--2; the
`full_drop0` seed-2 outlier (1.61 ms) was re-run (0.419 ms) and the re-run is used.

| row | p50 mean±sd | p95 mean | fallback | missing |
|---|---:|---:|---:|---:|
| version equality | 22.37±0.42 | 31.2 | 200/200 | 0 |
| cert., no log | 21.67±1.24 | 31.0 | 200/200 | 0 |
| graph+log, no check | 0.44±0.04 | 1.56 | 0 | 3 |
| cert.+log | 0.45±0.02 | 1.56 | 0 | 3 |
| graph+log, no check, lost | 0.47±0.04 | 3.9 | 0 | 3 |
| cert.+log, lost | 21.76±1.24 | 31.5 | 182/200 | 0 |
