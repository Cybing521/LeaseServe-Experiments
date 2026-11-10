"""Generate a 1M x 768d real-embedding corpus: Wikipedia paragraphs embedded
with all-mpnet-base-v2 (sentence-transformers), for the paper's 768d crossover.

Output (under OUT_DIR):
  base.f32     1,000,000 x 768 float32, L2-normalized
  queries.f32     10,000 x 768 float32, L2-normalized (disjoint paragraphs)
  rows_done.txt  checkpoint (paragraphs embedded so far)
  gen.log        progress log

Resumable: re-run skips rows already in the memmap checkpoint.
"""
import glob
import os
import time

import numpy as np
import torch

DEVICE = "cuda" if torch.cuda.is_available() else "cpu"
if DEVICE == "cpu":
    torch.set_num_threads(32)
# GPU fp16 inference, fp32 normalized embeddings out; CPU falls back to fp32.
DTYPE = torch.float16 if DEVICE == "cuda" else torch.float32

N = 1_010_000          # 1M base + 10k queries
BASE_N = 1_000_000
DIM = 768
OUT_DIR = "/root/work/icde/real768"
MM_PATH = f"{OUT_DIR}/all.f32"
CKPT = f"{OUT_DIR}/rows_done.txt"
LOG = f"{OUT_DIR}/gen.log"

os.makedirs(OUT_DIR, exist_ok=True)
logf = open(LOG, "a", buffering=1)


def log(msg):
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    logf.write(line + "\n")
    print(line, flush=True)


rows_done = 0
if os.path.exists(CKPT):
    rows_done = int(open(CKPT).read().strip() or 0)
log(f"start/resume at row {rows_done}")

mode = "r+" if os.path.exists(MM_PATH) else "w+"
mm = np.memmap(MM_PATH, dtype=np.float32, mode=mode, shape=(N, DIM))

from sentence_transformers import SentenceTransformer  # noqa: E402
from datasets import load_dataset  # noqa: E402

LOCAL_MODEL = "/root/work/icde/real768/mpnet"
if os.path.isdir(LOCAL_MODEL) and os.path.exists(f"{LOCAL_MODEL}/model.safetensors"):
    model = SentenceTransformer(LOCAL_MODEL, device=DEVICE, model_kwargs={"torch_dtype": DTYPE})
else:
    model = SentenceTransformer("sentence-transformers/all-mpnet-base-v2",
                                device=DEVICE, model_kwargs={"torch_dtype": DTYPE})
log(f"model loaded device={DEVICE}")

PARQUET_GLOB = "/root/work/icde/real768/shards/*.parquet"
if glob.glob(PARQUET_GLOB):
    ds = load_dataset("parquet", data_files=PARQUET_GLOB, split="train")
    log(f"loaded local parquet: {len(ds)} articles")
else:
    ds = load_dataset("wikimedia/wikipedia", "20231101.en", split="train", streaming=True)


def paragraphs():
    """Yield qualifying paragraphs; skip the first `rows_done` on resume."""
    emitted = 0
    n_articles = 0
    for art in ds:
        n_articles += 1
        for p in art["text"].split("\n\n"):
            p = p.strip()
            if 200 <= len(p) <= 1200:
                emitted += 1
                if emitted <= rows_done:
                    continue
                yield p
        if n_articles % 5000 == 0:
            log(f"articles={n_articles} emitted={emitted - rows_done} (+{emitted} total)")


BATCH = 512 if DEVICE == "cuda" else 256
FLUSH_EVERY = 64  # batches -> checkpoint every ~32k rows (GPU) / 16k (CPU)
buf = []
batches = 0
t0 = time.time()
t_checkpoint = time.time()

for p in paragraphs():
    buf.append(p)
    if len(buf) < BATCH:
        continue
    emb = model.encode(buf, batch_size=BATCH, convert_to_numpy=True,
                       normalize_embeddings=True, show_progress_bar=False)
    take = min(len(emb), N - rows_done)
    mm[rows_done:rows_done + take] = emb[:take]
    rows_done += take
    buf = []
    batches += 1
    if batches % FLUSH_EVERY == 0 or rows_done >= N:
        mm.flush()
        with open(CKPT, "w") as f:
            f.write(str(rows_done))
        if rows_done >= N:
            break
    if rows_done % 50_000 < BATCH:
        rate = rows_done / (time.time() - t_checkpoint + 1e-9)
        eta = (N - rows_done) / max(rate, 1.0)
        log(f"rows={rows_done}/{N} rate={rate:.0f}/s eta={eta/60:.0f}min")

mm.flush()
with open(CKPT, "w") as f:
    f.write(str(rows_done))

if rows_done < N:
    log(f"INCOMPLETE: only {rows_done}/{N} rows (source exhausted?)")
    raise SystemExit(1)

np.asarray(mm[:BASE_N]).tofile(f"{OUT_DIR}/base.f32")
np.asarray(mm[BASE_N:N]).tofile(f"{OUT_DIR}/queries.f32")
log(f"DONE base={BASE_N} queries={N - BASE_N} dim={DIM} "
    f"({os.path.getsize(f'{OUT_DIR}/base.f32')/1e9:.2f} GB base)")
log("WROTE base.f32 queries.f32")
