"""Integration: the 25 RiteSmith benchmark tasks, executed and type-checked by LunarDyson.

Reads the task definitions (read-only) from the RiteSmith repo. Each task's
Lua tool stub becomes a Python tool that evaluates the stub in a second
Runtime — the stubs are written in the common Lua/Luau subset.

    LD_BENCH_TASKS=../ritesmith/benchmarks/luau_codegen/tasks pytest -k bench
"""

import os
import sys
from pathlib import Path

import pytest

import lunardyson as ld

DEFAULT_TASKS = Path(__file__).resolve().parents[4] / "ritesmith" / "benchmarks" / "luau_codegen" / "tasks"
TASKS_DIR = Path(os.environ.get("LD_BENCH_TASKS", DEFAULT_TASKS))
CONTEXT = {"now": "2026-09-30T12:00:00Z"}


def _load_tasks() -> list[dict]:
    if not (TASKS_DIR / "__init__.py").exists():
        return []
    repo_root = TASKS_DIR.parents[2]
    sys.path.insert(0, str(repo_root))
    try:
        from benchmarks.luau_codegen.tasks import ALL_TASKS
    finally:
        sys.path.remove(str(repo_root))
    return ALL_TASKS


TASKS = _load_tasks()
pytestmark = pytest.mark.skipif(not TASKS, reason=f"benchmark tasks not found at {TASKS_DIR}")


def compare(expected, actual, path="$"):
    """Same rules as the benchmark: expected keys only, None = absent, {} == [], 1e-6 tolerance."""
    if isinstance(expected, dict):
        actual = {} if actual == [] else actual
        if not isinstance(actual, dict):
            return f"{path}: expected object, got {actual!r}"
        for key, value in expected.items():
            if value is None:
                if actual.get(key) is not None:
                    return f"{path}.{key}: expected absent, got {actual[key]!r}"
            elif key not in actual:
                return f"{path}.{key}: missing"
            elif mismatch := compare(value, actual[key], f"{path}.{key}"):
                return mismatch
        return None
    if isinstance(expected, list):
        actual = [] if actual == {} else actual
        if not isinstance(actual, list) or len(actual) != len(expected):
            return f"{path}: expected {expected!r}, got {actual!r}"
        for i, (e, a) in enumerate(zip(expected, actual, strict=True)):
            if mismatch := compare(e, a, f"{path}[{i}]"):
                return mismatch
        return None
    if isinstance(expected, bool):
        return None if actual is expected else f"{path}: expected {expected}, got {actual!r}"
    if isinstance(expected, (int, float)):
        ok = isinstance(actual, (int, float)) and not isinstance(actual, bool)
        return None if ok and abs(actual - expected) <= 1e-6 * max(1, abs(expected)) else f"{path}: {actual!r}"
    return None if actual == expected else f"{path}: expected {expected!r}, got {actual!r}"


class StubTool:
    """Evaluates a task's Lua stub (`function(args) ... end`) in its own runtime."""

    def __init__(self, stub: str):
        self.source = f"local stub = {stub.strip()}\nfunction run(input) return stub(input) end"
        self.runtime = ld.Runtime()
        self.calls = 0

    def __call__(self, args):
        self.calls += 1
        result = self.runtime.execute(self.source, args)
        if not result.ok:
            raise RuntimeError(f"stub failed: {result.message}")
        return result.output


def build_runtime(task: dict) -> tuple[ld.Runtime, dict[str, StubTool]]:
    rt = ld.Runtime()
    rt.declare_types("type Context = { now: string }\n" + task.get("types", ""))
    stubs = {}
    for tool in task.get("tools", []):
        stubs[tool["name"]] = StubTool(tool["stub"])
        rt.tool(tool["name"], stubs[tool["name"]], signature=tool["signature"], max_calls=tool.get("max_calls"))
    return rt, stubs


@pytest.mark.parametrize("task", TASKS, ids=[t["id"] for t in TASKS])
def test_bench_reference_executes(task):
    rt, stubs = build_runtime(task)
    for i, case in enumerate(task["test_cases"], start=1):
        for stub in stubs.values():
            stub.calls = 0
        result = rt.execute(task["reference"], case["input"], case.get("context", CONTEXT))
        assert result.ok, f"case {i}: {result.error_kind}: {result.message}"
        assert (mismatch := compare(case["expected"], result.output)) is None, f"case {i}: {mismatch}"
        for name, count in case.get("expected_calls", {}).items():
            assert stubs[name].calls == count, f"case {i}: {name} called {stubs[name].calls}x"


@pytest.mark.parametrize("task", TASKS, ids=[t["id"] for t in TASKS])
def test_bench_reference_typechecks_strict(task):
    rt, _ = build_runtime(task)
    diags = [d for d in rt.check(task["reference"], strict=True) if d["severity"] == "error"]
    assert diags == [], diags
