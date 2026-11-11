# Online 30k inputs

`queries_30k.f32` is the official 10k query set repeated three times; `qroles_30k.bin` assigns
all 30k queries to role 49 (the churn role). Run `python3 make_queries.py` from this directory to
regenerate both files. `queries_30k.f32` is 15 MB and is not committed; `qroles_30k.bin` is.
