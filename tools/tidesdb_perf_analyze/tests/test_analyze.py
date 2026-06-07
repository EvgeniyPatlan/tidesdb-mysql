"""Unit tests for the offline analyser."""
import json
import os
import struct
import tempfile

import pytest


SAMPLE_STRUCT = "<BBH4xQQ"
SAMPLE_SIZE = 24


def write_samples(path, samples):
    """samples: list of (method_id, thread_id, enter_tsc, exit_tsc)."""
    with open(path, "wb") as f:
        for m, t, e, x in samples:
            f.write(struct.pack(SAMPLE_STRUCT, m, t, 0, e, x))


def write_meta(path, tsc_ghz=3.5, capacity=65536, method_count=32, pid=1):
    with open(path, "w") as f:
        json.dump({
            "format_version": 1,
            "tsc_ghz": tsc_ghz,
            "capacity": capacity,
            "method_count": method_count,
            "pid": pid,
        }, f)


def test_histogram_from_samples():
    from tidesdb_perf_analyze import analyse
    with tempfile.TemporaryDirectory() as d:
        # Synthesise: 1000 samples for write_row with cycle deltas 100,200,...,100000.
        samples = [(1, 1, 0, delta) for delta in range(100, 100_001, 100)]
        write_samples(os.path.join(d, "1-01-write_row.bin"), samples)
        write_meta(os.path.join(d, "1-meta.json"))
        report = analyse(d)
        assert "write_row" in report
        wr = report["write_row"]
        assert wr["calls"] == 1000
        # cycles -> ns: 1 cycle / 3.5 GHz = ~0.286 ns; p50 of cycle deltas
        # 100..100000 step 100 = ~50050 cycles = ~14300 ns.
        assert 14000 < wr["p50_ns"] < 15000
        # p99 = ~99100 cycles = ~28310 ns
        assert 28000 < wr["p99_ns"] < 29000


def test_multi_file_merge():
    from tidesdb_perf_analyze import analyse
    with tempfile.TemporaryDirectory() as d:
        # Two .bin files for the same method (e.g. two processes); merge to totals.
        write_samples(os.path.join(d, "1-01-write_row.bin"),
                      [(1, 1, 0, 1000)] * 500)
        write_samples(os.path.join(d, "2-01-write_row.bin"),
                      [(1, 1, 0, 1000)] * 500)
        write_meta(os.path.join(d, "1-meta.json"))
        write_meta(os.path.join(d, "2-meta.json"))
        report = analyse(d)
        assert report["write_row"]["calls"] == 1000


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
