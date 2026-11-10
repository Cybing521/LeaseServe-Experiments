# OpenAlex ACL table (Table V)

Source: `llm_01:/root/autodl-tmp/icde_cm/out/oa/` (990k documents, 1,066 institutions,
ten head leases). `exact_p50/summary.txt` gives the exact row (`static_p50=1.86215`,
`static_p95=3.37789`); `lease_head10_m15|m30` and `lease_zipf_m15|m30` give the four lease rows
(`incr_p50`). Three repetitions per arm.

`../oa_replay_20261107/` is a separate churn replay (`b1_concurrent_v1`), not the source of this
table. The ACL is rebuilt by `experiments/cpp/fetch_openalex_acl.py` and
`gen_openalex_acl.py`; the raw OpenAlex dump is not shipped.
