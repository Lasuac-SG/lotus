#!/usr/bin/env python3
"""Run the checked-in serial SPDS/AffineSPDS benchmark cases."""

from __future__ import annotations

import argparse
import json
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = (
    ROOT / "benchmarks/real-world/CFL/InterleavedDyck/benchmark-cases.tsv"
)
BINARIES = {
    "spds": "lotus-cfl-interleaved-dyck-spds",
    "affine": "lotus-cfl-interleaved-dyck-affine-spds",
}


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument(
        "--engine", choices=tuple(BINARIES), action="append",
        help="engine to run; repeat for both (default: spds)",
    )
    parser.add_argument(
        "--timeout", type=float, default=0,
        help="per-case timeout in seconds; zero disables the timeout",
    )
    parser.add_argument(
        "--filter", help="run only manifest entries whose path contains this text",
    )
    parser.add_argument(
        "--scope", choices=("all-pairs", "source", "target"),
        help="run only manifest entries with this scope",
    )
    parser.add_argument(
        "--anchor", help="run only entries with this exact anchor",
    )
    parser.add_argument("--json", type=Path, help="also write structured results")
    return parser.parse_args()


def read_manifest(path: Path) -> list[tuple[Path, str, str | None]]:
    cases = []
    with path.open(encoding="utf-8") as source:
        for line_number, raw in enumerate(source, 1):
            line = raw.partition("#")[0].strip()
            if not line:
                continue
            fields = line.split()
            if len(fields) not in (2, 3):
                raise ValueError(f"{path}:{line_number}: expected DATASET SCOPE [ANCHOR]")
            dataset, scope = fields[:2]
            anchor = fields[2] if len(fields) == 3 else None
            if scope == "all-pairs" and anchor is not None:
                raise ValueError(f"{path}:{line_number}: all-pairs takes no anchor")
            if scope in ("source", "target") and anchor is None:
                raise ValueError(f"{path}:{line_number}: {scope} requires an anchor")
            if scope not in ("all-pairs", "source", "target"):
                raise ValueError(f"{path}:{line_number}: invalid scope {scope}")
            cases.append((ROOT / dataset, scope, anchor))
    return cases


def parse_output(output: str) -> dict[str, int]:
    values = {}
    names = {
        "candidate-pairs": "candidate_pairs",
        "states": "states",
        "transitions": "transitions",
        "weight-updates": "updates",
        "processed": "processed",
        "rules": "rules",
        "matrix-dimension": "matrix_dimension",
        "max-affine-rank": "max_rank",
        "coordinate-dimension": "coordinate_dimension",
        "slice-cache-hits": "slice_cache_hits",
        "compiled-rules": "compiled_rules",
        "prepared-weights": "prepared_weights",
        "matrix-products": "matrix_products",
        "basis-reductions": "basis_reductions",
        "basis-insertions": "basis_insertions",
        "cow-detaches": "cow_detaches",
        "intersection-tests": "intersection_tests",
        "observer-us": "observer_us",
        "intersection-us": "intersection_us",
        "certificate-us": "certificate_us",
        "setup-us": "setup_us",
        "saturation-us": "saturation_us",
        "readout-us": "readout_us",
        "projection-us": "projection_us",
    }
    for line in output.splitlines():
        key, separator, value = line.partition(":")
        if separator and key in names:
            try:
                values[names[key]] = int(value.strip())
            except ValueError:
                pass
    return values


def run_case(
    binary: Path, engine: str, dataset: Path, scope: str,
    anchor: str | None, timeout: float,
) -> dict[str, object]:
    command = [str(binary), f"--{scope}"]
    if anchor is not None:
        command.append(anchor)
    command.append("--timings")
    command.append(str(dataset))
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command, text=True, capture_output=True, check=False,
            timeout=timeout or None,
        )
        elapsed = time.perf_counter() - started
        status = "ok" if completed.returncode == 0 else f"exit-{completed.returncode}"
        result = parse_output(completed.stdout)
        if completed.returncode != 0:
            result["error"] = completed.stderr.strip()
    except subprocess.TimeoutExpired:
        elapsed = time.perf_counter() - started
        status = "timeout"
        result = {}
    return {
        "engine": engine,
        "dataset": str(dataset.relative_to(ROOT)),
        "scope": scope,
        "anchor": anchor,
        "status": status,
        "seconds": round(elapsed, 6),
        **result,
    }


def main() -> int:
    args = arguments()
    engines = args.engine or ["spds"]
    cases = read_manifest(args.manifest)
    if args.filter:
        cases = [case for case in cases if args.filter in str(case[0])]
    if args.scope:
        cases = [case for case in cases if case[1] == args.scope]
    if args.anchor:
        cases = [case for case in cases if case[2] == args.anchor]
    results = []
    columns = [
        "engine", "dataset", "scope", "anchor", "status", "seconds",
        "candidate_pairs", "states", "transitions", "updates", "processed",
        "rules", "matrix_dimension", "max_rank",
        "setup_us", "saturation_us", "readout_us", "projection_us",
    ]
    print("\t".join(columns), flush=True)
    for engine in engines:
        binary = args.build_dir / "bin" / BINARIES[engine]
        if not binary.is_file():
            raise FileNotFoundError(f"missing benchmark binary: {binary}")
        for dataset, scope, anchor in cases:
            result = run_case(binary, engine, dataset, scope, anchor, args.timeout)
            results.append(result)
            values = (result.get(column, "") for column in columns)
            print("\t".join("" if value is None else str(value) for value in values),
                  flush=True)
    if args.json:
        args.json.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
