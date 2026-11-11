#!/usr/bin/env python3
"""Regenerate the 30k-query inputs: official 10k queries repeated 3x, all role 49."""
import struct
from pathlib import Path
here = Path(__file__).resolve().parent
root = here.parents[2]
q = (root / "experiments/results/ab_stream_official10k_20261109/queries10k_real.f32").read_bytes()
(here / "queries_30k.f32").write_bytes(q * 3)
with open(here / "qroles_30k.bin", "wb") as f:
    f.write(struct.pack("<i", 30000))
    for _ in range(30000):
        f.write(struct.pack("<i", 49))
