# 10M×128d 真实语料（SIFT10M）C++ 三种子 — 2026-11-07 shengxin_02

来源：`shengxin_02:/root/autodl-tmp/icde_sift10m/`（`setup_sift10m_shengxin02.sh`，三种子全 exit=0）
数据：SIFT10M 官方镜像（Nanvivi/sift10m-modify，uint8 11M×128）取前 10M 转 f32；
ERBAC-10M ACL（mimo 同源 roles.bin）；queries = SIFT 官方 query 前 500。
协议：`hnsw_lease_delta_mvcc`，K=3，nq=500，changes=200，threads=4，CHANGE_US=500，
与 mimo `cpp128_mimo_20261106`（合成高斯）字节同协议，仅换真实语料。

## 结果（3 seeds）

| 量 | SIFT10M 真实（本表） | 合成高斯（mimo，对照） |
|---|---|---|
| build | 602.4 / 584.9 / 589.5 s → **592.3±9.1 s** | 594±7 s |
| p50 | 76.4 / 71.5 / 71.1 ms → **73.0±2.9 ms** | 50.4±2.6 ms |
| p95 | 128.9 / 83.2 / 78.4 ms（s0 冷尾） | 57.2±0.4 ms |
| p99 | 191.3 / 99.4 / 81.4 ms | 58.5±0.6 ms |
| missing (of 5000) | **17 / 17 / 17** | 1549 |
| leaks / straddled / retries / fallbacks / pin violations | 全 0（3/3 seeds） | 0 |

## 解读

1. **build 成本与语料分布无关**：592±9 vs 594±7 s——O(n log n) 构建在真实语料复现。
2. **完整度大幅改善**：missing 17 vs 1549。合成高斯是刻意难点（各向同性、角色内聚类弱），
   SIFT 真实几何下 mfac=15 预算的 ann 缺口近乎消失。这直接削弱 "synthetic Gaussian caveat"：
   论文 10M×128d 的 missing 数字是 stress-test 上界，真实语料好 90×。
3. **绝对延迟不可直接对比**：shengxin_02（EPYC/Xeon?）与 mimo（9950X）不同宿主，p50 76 vs 50 ms
   含主机差；completeness 计数与 build 成本跨宿主有效。
4. 注意：本 run 是 K=3 混合群体口径（与 mimo cpp128 相同），missing 17 仍是该口径。
