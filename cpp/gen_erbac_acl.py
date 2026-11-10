"""Regenerate the real ERBAC ACL (variable role sizes) for the C++ lease drivers.

`gen_role_acl.py` makes every role the same size, which is what the decoupling
probe needs but not what ERBAC is.  This script calls `rbac_workload.erbac` with
the parameters used throughout the paper (40 functional roles, 100 business
roles, mp_factor 25, seed 0) and writes the binary layout the C++ drivers read.

Two query workloads are written:
  qroles_head10.bin  uniform over the ten largest roles (the paper's read-path
                     convention: every query targets a head lease)
  qroles_all.bin     uniform over all business roles (the archived harness
                     convention: 10% lease coverage, 90% exact fallback)

Run:
  python3 gen_erbac_acl.py --n 1000000 --nq 500 --out acl/erbac100
"""
import argparse
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "mechanism"))
from rbac_workload import erbac  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=1000000)
    ap.add_argument("--nq", type=int, default=500)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    role_docs, _users = erbac(a.n, n_functional=40, n_business=100, mfr=3, mbr=3,
                              mp_factor=25, n_users=1000, seed=a.seed)
    sizes = np.array([len(d) for d in role_docs], dtype=np.int64)
    R = len(role_docs)
    os.makedirs(a.out, exist_ok=True)

    with open(os.path.join(a.out, "roles.bin"), "wb") as f:
        f.write(np.asarray([R], dtype=np.int32).tobytes())
        for r, docs in enumerate(role_docs):
            ids = np.sort(np.asarray(docs, dtype=np.int64)).astype(np.int32)
            f.write(np.asarray([r, len(ids)], dtype=np.int32).tobytes())
            f.write(ids.tobytes())

    by_size = np.argsort(-sizes)
    rng = np.random.default_rng(a.seed + 11)
    head = rng.choice(by_size[:10], size=a.nq, replace=True).astype(np.int32)
    allq = rng.choice(R, size=a.nq, replace=True).astype(np.int32)
    for name, q in [("qroles_head10.bin", head), ("qroles_all.bin", allq)]:
        with open(os.path.join(a.out, name), "wb") as f:
            f.write(np.asarray([len(q)], dtype=np.int32).tobytes())
            f.write(q.tobytes())

    meta = dict(n=a.n, roles=R, nq=a.nq, seed=a.seed,
                role_size=dict(min=int(sizes.min()), median=float(np.median(sizes)),
                               mean=round(float(sizes.mean()), 1), max=int(sizes.max())),
                top10_sizes=[int(x) for x in sizes[by_size[:10]]],
                top10_share_of_vectors=round(float(sizes[by_size[:10]].sum() / a.n), 4))
    with open(os.path.join(a.out, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)
    print(json.dumps(meta))


if __name__ == "__main__":
    main()
