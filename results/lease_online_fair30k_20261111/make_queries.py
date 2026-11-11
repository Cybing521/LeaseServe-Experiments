#!/usr/bin/env python3
"""Build the 30k-query / all-role-49 inputs by repeating the official 10k set three times."""
import struct
from pathlib import Path
root = Path(__file__).resolve().parents[3]
q = (root / "experiments/results/ab_stream_official10k_20261109/queries10k_real.f32").read_bytes()
Path("queries_30k.f32").write_bytes(q * 3)
with open("qroles_30k.bin", "wb") as f:
    f.write(struct.pack("<i", 30000))
    for _ in range(30000):
        f.write(struct.pack("<i", 49))
