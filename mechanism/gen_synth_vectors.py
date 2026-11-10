#!/usr/bin/env python3
"""Generate synthetic float32 vectors (bare layout, d32/d33 compatible).

base.f32    n x dim float32, N(0, 1), unit-normalized rows (cosine == l2 rank)
queries.f32 nq x dim float32, same distribution, disjoint draw

Run:
  python3 gen_synth_vectors.py --n 100000 --dim 128 --nq 500 --seed 0 \
      --out ../cpp/acl/synth100k/
"""
import argparse
import os

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=100000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--nq", type=int, default=500)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    rng = np.random.default_rng(a.seed)
    base = rng.standard_normal((a.n, a.dim), dtype=np.float32)
    base /= np.linalg.norm(base, axis=1, keepdims=True)
    qs = rng.standard_normal((a.nq, a.dim), dtype=np.float32)
    qs /= np.linalg.norm(qs, axis=1, keepdims=True)
    base.tofile(os.path.join(a.out, "base.f32"))
    qs.tofile(os.path.join(a.out, "queries.f32"))
    print(dict(n=a.n, dim=a.dim, nq=a.nq, seed=a.seed,
               out=os.path.abspath(a.out)))


if __name__ == "__main__":
    main()
