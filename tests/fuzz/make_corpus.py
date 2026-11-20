#!/usr/bin/env python3
"""Seed corpora for the fuzzers from the conformance suite and, when present, the
RiteSmith Luau benchmark references (LD_BENCH_TASKS or ../ritesmith/...).

    python3 tests/fuzz/make_corpus.py build-fuzz/corpus
"""

import hashlib
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SEP = "\n--8<--\n"
REPLIES = [
    '{"ok":true,"v":7,"name":"x"}',
    '{"ok":false,"error":"not_found"}',
    "!tool exploded",
    '[1,2,3]',
    '{"ok":true,"items":[{"sku":"A","qty":2}]}',
]


def sources() -> list[str]:
    cases = json.loads((ROOT / "tests/conformance/cases.json").read_text())["cases"]
    out = [c["source"] for c in cases]
    tasks = Path(os.environ.get("LD_BENCH_TASKS", ROOT.parent / "ritesmith/benchmarks/luau_codegen/tasks"))
    if (tasks / "__init__.py").exists():
        sys.path.insert(0, str(tasks.parents[2]))
        from benchmarks.luau_codegen.tasks import ALL_TASKS  # noqa: PLC0415

        out += [t["reference"] for t in ALL_TASKS if t.get("reference")]
    return out


def write(directory: Path, blob: bytes) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / hashlib.sha1(blob).hexdigest()).write_bytes(blob)


def main() -> None:
    base = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build-fuzz/corpus")
    srcs = sources()
    for src in srcs:
        write(base / "exec", src.encode())
        write(base / "exec", (src + SEP + SEP.join(REPLIES)).encode())
        write(base / "check", b"\x01" + src.encode())  # strict
        write(base / "check", b"\x00" + src.encode())  # nonstrict
    print(f"{len(srcs)} programs -> {base}/exec, {base}/check")


if __name__ == "__main__":
    main()
