"""Runs the language-neutral conformance suite (tests/conformance/cases.json)."""

import json
from pathlib import Path

import pytest

import lunardyson as ld

CASES = json.loads(
    (Path(__file__).resolve().parents[3] / "tests" / "conformance" / "cases.json").read_text()
)["cases"]


def make_runtime(case: dict) -> ld.Runtime:
    limits = case.get("limits", {})
    rt = ld.Runtime(
        memory_mb=limits.get("memory_mb", 16),
        cpu_time_ms=limits.get("cpu_time_ms", 1000),
        max_effects=limits.get("max_effects"),
    )
    for tool in case.get("tools", []):
        if "returns_string_bytes" in tool:
            value = "x" * tool["returns_string_bytes"]
        else:
            value = tool.get("returns")
        rt.tool(
            tool["name"],
            lambda _args, value=value: value,
            effect=tool.get("effect", "read"),
            max_calls=tool.get("max_calls"),
            max_response_bytes=tool.get("max_response_bytes"),
        )
    return rt


def check_expectation(result: ld.Result, expect: dict) -> None:
    if "error_kind" in expect:
        assert not result.ok, f"expected error {expect['error_kind']}, got output {result.output}"
        assert result.error_kind in expect["error_kind"], f"{result.error_kind}: {result.message}"
    if "output_equals" in expect:
        assert result.ok, f"{result.error_kind}: {result.message}"
        assert result.output == expect["output_equals"]
    if "tool_calls" in expect:
        assert result.stats.tool_calls == expect["tool_calls"]
    if "vm_time_ms_between" in expect:
        low, high = expect["vm_time_ms_between"]
        assert low * 1000 <= result.stats.vm_time_us <= high * 1000, result.stats


@pytest.mark.parametrize("case", CASES, ids=[c["name"] for c in CASES])
def test_conformance(case):
    with make_runtime(case) as rt:
        result = rt.execute(case["source"], case.get("input"), case.get("context"))
        check_expectation(result, case["expect"])
        if "then" in case:
            follow = case["then"]
            check_expectation(rt.execute(follow["source"]), follow["expect"])
