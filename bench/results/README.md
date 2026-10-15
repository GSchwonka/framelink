# Raw measurement results

One file per run, named `<test>_<target>_<date>`, exactly as the scripts wrote it. These are the source of every number in [docs/MEASUREMENTS.md](../../docs/MEASUREMENTS.md).

| Script | Writes |
|--------|--------|
| `bench/esp32_collect.py --jsonl FILE` | ESP32 benchmark and loopback results, one JSON object per line |
| `bench/host_recovery.py --csv FILE` | Recovery under injected corruption |
