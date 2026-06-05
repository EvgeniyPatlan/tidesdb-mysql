"""tidesdb-perf-analyze: read per-method .bin sample files and emit reports."""
from __future__ import annotations

import argparse
import glob
import json
import os
import struct

import numpy as np

SAMPLE_STRUCT = "<BBHQQ"
SAMPLE_SIZE = 24

METHOD_NAMES = {
    1: "write_row", 2: "update_row", 3: "delete_row",
    4: "index_read_map", 5: "index_next", 6: "index_prev",
    7: "rnd_next", 8: "rnd_pos",
    9: "external_lock", 10: "start_stmt", 11: "store_lock",
    12: "commit", 13: "rollback",
    14: "savepoint_set", 15: "savepoint_release", 16: "savepoint_rollback",
    17: "open", 18: "close", 19: "info", 20: "table_flags_cache_init",
    21: "create", 22: "delete_table",
    23: "check_if_supported_inplace_alter", 24: "prepare_inplace_alter_table",
    25: "inplace_alter_table", 26: "commit_inplace_alter_table",
    27: "serialize_row", 28: "deserialize_row",
    29: "key_copy_to_comparable", 30: "pk_from_record",
    31: "encrypt_row_into", 32: "decrypt_row",
}


def load_meta(d: str) -> dict:
    """Locate and parse the first *-meta.json file in directory d."""
    metas = glob.glob(os.path.join(d, "*-meta.json"))
    if not metas:
        raise FileNotFoundError(f"No *-meta.json in {d}")
    with open(metas[0]) as f:
        return json.load(f)


def load_method_file(path: str) -> np.ndarray:
    """Return Nx5 array (method_id, thread_id, reserved, enter, exit)."""
    sz = os.path.getsize(path)
    n = sz // SAMPLE_SIZE
    if n == 0:
        return np.empty((0, 5), dtype=np.uint64)
    parsed = np.empty((n, 5), dtype=np.uint64)
    with open(path, "rb") as f:
        for i in range(n):
            b = f.read(SAMPLE_SIZE)
            m, t, r, e, x = struct.unpack(SAMPLE_STRUCT, b)
            parsed[i] = (m, t, r, e, x)
    return parsed


def analyse(directory: str) -> dict:
    """Return {method_name: {calls, mean_ns, p50_ns, p95_ns, p99_ns, max_ns, total_ms}}."""
    meta = load_meta(directory)
    tsc_ghz = float(meta["tsc_ghz"])
    by_method: dict = {}
    for path in glob.glob(os.path.join(directory, "*-*-*.bin")):
        if path.endswith("meta.json") or "calibration" in os.path.basename(path):
            continue
        arr = load_method_file(path)
        if arr.size == 0:
            continue
        method_id = int(arr[0, 0])
        by_method.setdefault(method_id, []).append(arr)
    out: dict = {}
    for mid, arrs in by_method.items():
        a = np.concatenate(arrs)
        deltas_cycles = a[:, 4] - a[:, 3]
        ns = (deltas_cycles.astype(np.float64) / tsc_ghz)
        name = METHOD_NAMES.get(mid, f"method_{mid}")
        out[name] = {
            "calls": int(len(ns)),
            "mean_ns": float(ns.mean()),
            "p50_ns": float(np.percentile(ns, 50)),
            "p95_ns": float(np.percentile(ns, 95)),
            "p99_ns": float(np.percentile(ns, 99)),
            "max_ns": float(ns.max()),
            "total_ms": float(ns.sum() / 1e6),
        }
    return out


def to_markdown(report: dict) -> str:
    """Render an analyse() report dict as a markdown table sorted by total_ms desc."""
    rows = sorted(report.items(), key=lambda kv: kv[1]["total_ms"], reverse=True)
    lines = [
        "| method | calls | total_ms | mean_us | p50_us | p95_us | p99_us | max_us |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for name, m in rows:
        lines.append(
            f"| {name} | {m['calls']:,} | {m['total_ms']:.1f} | "
            f"{m['mean_ns']/1000:.2f} | {m['p50_ns']/1000:.2f} | "
            f"{m['p95_ns']/1000:.2f} | {m['p99_ns']/1000:.2f} | "
            f"{m['max_ns']/1000:.2f} |"
        )
    return "\n".join(lines) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser(prog="tidesdb-perf-analyze")
    p.add_argument("directory", help="Directory of .bin + meta.json files")
    p.add_argument("--output", default="-", help="Markdown output file (- for stdout)")
    args = p.parse_args(argv)
    report = analyse(args.directory)
    md = to_markdown(report)
    if args.output == "-":
        print(md)
    else:
        with open(args.output, "w") as f:
            f.write(md)


if __name__ == "__main__":
    main()
