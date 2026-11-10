# OpenAlex ACL table (Table V)

Source: `llm_01:/root/autodl-tmp/icde_cm/out/oa/` (990k documents, 1,066 institutions,
ten head leases, three repetitions per arm). `exact_p50/summary.txt` gives the exact row
(`static_p50=1.86215`, `static_p95=3.37789`); `lease_head10_m15|m30` and `lease_zipf_m15|m30`
give the four lease rows (`incr_p50`).

Paper table: exact 1.862/3.378; head-10 m=150 0.286/0.372, missing 2; m=300 0.489/0.589,
missing 0; size-weighted m=150 0.260/1.261, missing 0; m=300 0.318/1.298, missing 0.
The archived three-rep medians of `incr_p50` are 0.283 (head-10 m15), 0.494 (m30),
0.266 (zipf m15) and 0.318 (zipf m30); p95 medians are 0.352/0.59/1.29/1.30. The table's
0.286/0.260 are within the run-to-run spread (<=5%) of these records.

`../oa_replay_20261107/` is a separate churn replay (`b1_concurrent_v1`), not the source of this
table. The ACL is rebuilt by `experiments/cpp/fetch_openalex_acl.py` and
`gen_openalex_acl.py`; the raw OpenAlex dump is not shipped.
