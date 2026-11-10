# hnswlib headers for the native performance builds

These are the stock upstream hnswlib headers used by every native (non-sanitizer)
performance batch in the paper. Source: https://github.com/nmslib/hnswlib,
Apache-2.0 (see `LICENSE`). `hnswalg.h` md5 `ffa31f5c889a76bb69ceadd4cb83a594`.

Sanitizer builds use a patched `hnswalg.h` that adds bounds guards in
`addPointWithLevel` / `searchBaseLayer`; it ships as `hnswlib_fix/hnswalg.h` at the
package root (md5 `10348a1b5e24e790a7ca49af11fb4c7f`). Copy that file over this
one only for sanitizer runs; the performance numbers in the paper use the stock
header.

Build from the package root:

```bash
g++ -O3 -march=native -std=c++17 -I experiments/cpp/third_party \
    experiments/cpp/lease_certified.cpp -o lease_certified -pthread
```
