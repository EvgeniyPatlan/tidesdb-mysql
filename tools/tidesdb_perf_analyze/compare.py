"""tidesdb-perf-analyze --compare a/ b/: side-by-side diff."""
from __future__ import annotations

import argparse
import sys

from . import analyse


def to_diff_markdown(left: dict, right: dict, left_label="A", right_label="B") -> str:
    """Render a side-by-side (A, B, ΔA-B) markdown table sorted by absolute total delta."""
    methods = sorted(set(left) | set(right))
    rows = []
    for m in methods:
        l_ns = left.get(m, {}).get("mean_ns", 0.0)
        r_ns = right.get(m, {}).get("mean_ns", 0.0)
        calls = max(left.get(m, {}).get("calls", 0),
                    right.get(m, {}).get("calls", 0))
        delta_ns = r_ns - l_ns
        delta_total_ms = delta_ns * calls / 1e6
        rows.append((m, l_ns, r_ns, delta_ns, calls, delta_total_ms))
    rows.sort(key=lambda r: r[5], reverse=True)
    lines = [
        f"| method | {left_label} ns/call | {right_label} ns/call | Δ ns | calls | Δ total (ms) |",
        "|---|---|---|---|---|---|",
    ]
    for m, l, r, d, c, dt in rows:
        lines.append(f"| {m} | {l:.0f} | {r:.0f} | {d:+.0f} | {c:,} | {dt:+.1f} |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser()
    p.add_argument("left", help="Left capture dir")
    p.add_argument("right", help="Right capture dir")
    p.add_argument("--left-label", default="MySQL")
    p.add_argument("--right-label", default="MariaDB")
    p.add_argument("--output", default="-")
    args = p.parse_args(argv)
    left = analyse(args.left)
    right = analyse(args.right)
    md = to_diff_markdown(left, right, args.left_label, args.right_label)
    if args.output == "-":
        print(md)
    else:
        with open(args.output, "w") as f:
            f.write(md)


if __name__ == "__main__":
    main(sys.argv[1:])
