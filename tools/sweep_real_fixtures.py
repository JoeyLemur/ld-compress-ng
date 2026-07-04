#!/usr/bin/env python3
"""Run ld-compress-ng benchmark sweeps across ignored real LDS fixtures."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


DEFAULT_FIXTURE_DIR = (
    "reference/testdata/ld-decode-testdata-ci/"
    "1cf698d2025e8515e9ef57e34adaf85a194da96a"
)
CSV_COLUMNS = [
    "fixture",
    "backend",
    "threads",
    "frame_samples",
    "lpc_order",
    "lpc_precision",
    "rice_order",
    "input_bytes",
    "output_bytes",
    "samples",
    "ratio",
    "elapsed_s",
    "mib_per_s",
    "subframes",
    "lpc_orders",
    "rice_orders",
    "wasted_bits",
]
BENCH_COLUMNS = [
    "backend",
    "threads",
    "frame_samples",
    "lpc_order",
    "lpc_prec",
    "rice_order",
    "input_bytes",
    "output_bytes",
    "samples",
    "ratio",
    "elapsed_s",
    "mib_per_s",
    "subframes",
    "lpc_orders",
    "rice_orders",
    "wasted_bits",
]


@dataclass(frozen=True)
class SweepConfig:
    threads: str
    frame_samples: str
    lpc_order: str
    lpc_precision: str
    rice_partition_order: str


def parse_uint_list(text: str, name: str, minimum: int, maximum: int) -> list[int]:
    values: list[int] = []
    if not text:
        raise argparse.ArgumentTypeError(f"{name} cannot be empty")
    for item in text.split(","):
        if not item:
            raise argparse.ArgumentTypeError(f"{name} contains an empty item")
        try:
            value = int(item, 10)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"{name} contains a non-integer item: {item}") from exc
        if value < minimum or value > maximum:
            raise argparse.ArgumentTypeError(f"{name} values must be {minimum}..{maximum}: {value}")
        values.append(value)
    return values


def uint_list_arg(name: str, minimum: int, maximum: int):
    def parse(text: str) -> str:
        parse_uint_list(text, name, minimum, maximum)
        return text

    return parse


def find_fixtures(root: Path, limit: int | None) -> list[Path]:
    fixtures = sorted(path for path in root.rglob("*.lds") if path.is_file())
    if limit is not None:
        fixtures = fixtures[:limit]
    return fixtures


def bench_command(binary: Path, config: SweepConfig, fixture: Path) -> list[str]:
    return [
        str(binary),
        "bench",
        "--threads",
        config.threads,
        "--frame-samples",
        config.frame_samples,
        "--lpc-order",
        config.lpc_order,
        "--lpc-precision",
        config.lpc_precision,
        "--rice-partition-order",
        config.rice_partition_order,
        str(fixture),
    ]


def parse_bench_output(output: str, fixture: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    lines = [line for line in output.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("bench produced no output")

    header = lines[0].split()
    if header != BENCH_COLUMNS:
        raise RuntimeError(
            "unexpected bench header: " + " ".join(header)
        )

    for line in lines[1:]:
        parts = line.split()
        if len(parts) != len(BENCH_COLUMNS):
            raise RuntimeError("could not parse bench row: " + line)
        row = dict(zip(BENCH_COLUMNS, parts))
        row["lpc_precision"] = row.pop("lpc_prec")
        row["fixture"] = fixture
        rows.append({column: row[column] for column in CSV_COLUMNS})
    return rows


def run_bench(binary: Path, config: SweepConfig, fixture: Path, root: Path) -> list[dict[str, str]]:
    command = bench_command(binary, config, fixture)
    completed = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        if completed.stdout:
            print(completed.stdout, end="", file=sys.stderr)
        if completed.stderr:
            print(completed.stderr, end="", file=sys.stderr)
        raise RuntimeError(f"bench failed for {fixture} with exit code {completed.returncode}")
    return parse_bench_output(completed.stdout, fixture.relative_to(root).as_posix())


def numeric(row: dict[str, str], column: str) -> float:
    return float(row[column])


def int_field(row: dict[str, str], column: str) -> int:
    return int(row[column])


def native_key(row: dict[str, str]) -> tuple[str, str, str, str, str]:
    return (
        row["threads"],
        row["frame_samples"],
        row["lpc_order"],
        row["lpc_precision"],
        row["rice_order"],
    )


def format_bytes(value: int) -> str:
    return f"{value:,}"


def sorted_native_rows(rows: Iterable[dict[str, str]]) -> list[dict[str, str]]:
    return sorted(
        (row for row in rows if row["backend"] == "native-fixed"),
        key=lambda row: (int_field(row, "output_bytes"), numeric(row, "elapsed_s")),
    )


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=CSV_COLUMNS)
        writer.writeheader()
        writer.writerows(rows)


def write_markdown(
    path: Path,
    rows: list[dict[str, str]],
    fixtures: list[Path],
    root: Path,
    config: SweepConfig,
) -> None:
    by_fixture: dict[str, list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        by_fixture[row["fixture"]].append(row)

    fixture_names = [fixture.relative_to(root).as_posix() for fixture in fixtures]
    cpu_by_fixture = {
        name: next(row for row in by_fixture[name] if row["backend"] == "cpu")
        for name in fixture_names
    }

    native_groups: dict[tuple[str, str, str, str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["backend"] == "native-fixed":
            native_groups[native_key(row)].append(row)

    complete_groups = [
        (key, group)
        for key, group in native_groups.items()
        if len(group) == len(fixture_names)
    ]
    complete_groups.sort(key=lambda item: (
        sum(int_field(row, "output_bytes") for row in item[1]),
        sum(numeric(row, "elapsed_s") for row in item[1]),
    ))

    total_cpu_bytes = sum(int_field(row, "output_bytes") for row in cpu_by_fixture.values())

    with path.open("w", encoding="utf-8") as output:
        output.write("# Real Fixture Native Tuning Sweep\n\n")
        output.write(f"- Fixtures: `{root}`\n")
        output.write(f"- Fixture count: {len(fixture_names)}\n")
        output.write(f"- Threads: `{config.threads}`\n")
        output.write(f"- Frame samples: `{config.frame_samples}`\n")
        output.write(f"- LPC orders: `{config.lpc_order}`\n")
        output.write(f"- LPC precisions: `{config.lpc_precision}`\n")
        output.write(f"- Rice partition orders: `{config.rice_partition_order}`\n\n")

        output.write("## Best Native Per Fixture\n\n")
        output.write("| Fixture | CPU bytes | Native bytes | Native ratio | Gap vs CPU | Settings |\n")
        output.write("| --- | ---: | ---: | ---: | ---: | --- |\n")
        for name in fixture_names:
            cpu = cpu_by_fixture[name]
            best = sorted_native_rows(by_fixture[name])[0]
            cpu_bytes = int_field(cpu, "output_bytes")
            native_bytes = int_field(best, "output_bytes")
            gap = ((native_bytes / cpu_bytes) - 1.0) * 100.0 if cpu_bytes else 0.0
            settings = (
                f"threads={best['threads']}, frame={best['frame_samples']}, "
                f"lpc={best['lpc_order']}, prec={best['lpc_precision']}, "
                f"rice={best['rice_order']}"
            )
            output.write(
                f"| `{name}` | {format_bytes(cpu_bytes)} | {format_bytes(native_bytes)} | "
                f"{float(best['ratio']):.4f} | {gap:+.2f}% | `{settings}` |\n"
            )

        output.write("\n## Aggregate Native Configs\n\n")
        output.write("| Rank | Native bytes | Gap vs CPU | Elapsed s | Settings |\n")
        output.write("| ---: | ---: | ---: | ---: | --- |\n")
        for rank, (key, group) in enumerate(complete_groups[:10], start=1):
            native_bytes = sum(int_field(row, "output_bytes") for row in group)
            elapsed = sum(numeric(row, "elapsed_s") for row in group)
            gap = ((native_bytes / total_cpu_bytes) - 1.0) * 100.0 if total_cpu_bytes else 0.0
            threads, frame_samples, lpc_order, lpc_precision, rice_order = key
            settings = (
                f"threads={threads}, frame={frame_samples}, lpc={lpc_order}, "
                f"prec={lpc_precision}, rice={rice_order}"
            )
            output.write(
                f"| {rank} | {format_bytes(native_bytes)} | {gap:+.2f}% | "
                f"{elapsed:.3f} | `{settings}` |\n"
            )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run ld-compress-ng bench across a tree of real LDS fixtures."
    )
    parser.add_argument("--binary", default="build/ld-compress-ng", type=Path)
    parser.add_argument("--fixtures", default=DEFAULT_FIXTURE_DIR, type=Path)
    parser.add_argument("--out-dir", default="build/real-fixture-sweeps", type=Path)
    parser.add_argument("--threads", default="1", type=uint_list_arg("threads", 1, 1024))
    parser.add_argument("--frame-samples", default="4608",
        type=uint_list_arg("frame samples", 16, 4608))
    parser.add_argument("--lpc-order", default="10,12",
        type=uint_list_arg("LPC order", 0, 12))
    parser.add_argument("--lpc-precision", default="10,12",
        type=uint_list_arg("LPC precision", 1, 15))
    parser.add_argument("--rice-partition-order", default="4",
        type=uint_list_arg("Rice partition order", 0, 8))
    parser.add_argument("--limit", type=int,
        help="benchmark only the first N fixtures after path sorting")
    parser.add_argument("--dry-run", action="store_true",
        help="print bench commands without running them")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    binary = args.binary
    fixture_root = args.fixtures

    if args.limit is not None and args.limit <= 0:
        raise RuntimeError("--limit must be positive")
    if not binary.is_file():
        raise RuntimeError(f"binary not found: {binary}")
    if not fixture_root.is_dir():
        raise RuntimeError(f"fixture directory not found: {fixture_root}")

    fixtures = find_fixtures(fixture_root, args.limit)
    if not fixtures:
        raise RuntimeError(f"no .lds fixtures found under {fixture_root}")

    config = SweepConfig(
        threads=args.threads,
        frame_samples=args.frame_samples,
        lpc_order=args.lpc_order,
        lpc_precision=args.lpc_precision,
        rice_partition_order=args.rice_partition_order,
    )

    if args.dry_run:
        for fixture in fixtures:
            print(" ".join(bench_command(binary, config, fixture)))
        return 0

    args.out_dir.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    csv_path = args.out_dir / f"real-fixture-sweep-{stamp}.csv"
    markdown_path = args.out_dir / f"real-fixture-sweep-{stamp}.md"

    rows: list[dict[str, str]] = []
    for index, fixture in enumerate(fixtures, start=1):
        rel = fixture.relative_to(fixture_root).as_posix()
        print(f"[{index}/{len(fixtures)}] {rel}", flush=True)
        fixture_rows = run_bench(binary, config, fixture, fixture_root)
        rows.extend(fixture_rows)
        best = sorted_native_rows(fixture_rows)[0]
        print(
            "    best native-fixed: "
            f"{best['output_bytes']} bytes, ratio {best['ratio']}, "
            f"frame={best['frame_samples']} lpc={best['lpc_order']} "
            f"prec={best['lpc_precision']} rice={best['rice_order']} "
            f"threads={best['threads']}",
            flush=True,
        )

    write_csv(csv_path, rows)
    write_markdown(markdown_path, rows, fixtures, fixture_root, config)
    print(f"wrote {csv_path}")
    print(f"wrote {markdown_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        print(f"sweep_real_fixtures.py: {exc}", file=sys.stderr)
        raise SystemExit(1)
