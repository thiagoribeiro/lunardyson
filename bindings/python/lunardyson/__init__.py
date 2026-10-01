"""Python binding for LunarDyson — execution runtime for untrusted Luau programs.

Two ways to run a program:

- ``Runtime.execute(source, input, context)``: tools registered with a Python
  callable are invoked automatically; returns a ``Result``.
- ``Runtime.start(...)``: step API. The returned ``Execution`` stops at every
  tool call (``status == "pending_tool"``); call ``resume(result)`` yourself.
  Use it for async tools, remote tools or effect journaling/replay.

A Runtime is not thread-safe; use one per thread (or a pool).
"""

import json
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

from lunardyson._ffi import UNLIMITED, ffi, lib

__all__ = ["UNLIMITED", "Call", "Execution", "LunarDysonError", "Result", "Runtime", "version"]

ERROR_KINDS = [
    "none",
    "syntax",
    "timeout",
    "memory",
    "tool_budget",
    "effect_budget",
    "unknown_tool",
    "runtime",
    "contract",
    "invalid_arg",
]
EFFECTS = {"pure": 0, "read": 1, "write": 2, "external_write": 3, "money": 4}
_EFFECT_NAMES = tuple(EFFECTS)
_STATUS = ("done", "pending_tool", "error")
_dumps = json.JSONEncoder(ensure_ascii=False, separators=(",", ":")).encode
_loads = json.loads


class LunarDysonError(Exception):
    pass


def version() -> str:
    return ffi.string(lib.ld_version()).decode()


@dataclass
class Call:
    call_id: int
    tool: str
    args: Any
    effect: str


@dataclass
class Stats:
    vm_time_us: int = 0
    peak_memory_bytes: int = 0
    tool_calls: int = 0


@dataclass
class Result:
    ok: bool
    output: Any = None
    error_kind: str = "none"
    message: str = ""
    stats: Stats = field(default_factory=Stats)


class Execution:
    """One program run, driven step by step."""

    def __init__(self, runtime: "Runtime", ptr, status: int):
        self._runtime = runtime  # keeps the runtime alive while the execution exists
        self._ptr = ptr
        self._status = status
        self._call = ffi.new("ld_call*")  # reused for every pending() query

    @property
    def status(self) -> str:
        return _STATUS[self._status]

    def pending(self) -> Call | None:
        call = self._call
        if not lib.ld_exec_pending(self._ptr, call):
            return None
        args = _loads(ffi.unpack(call.args_json, call.args_len))
        return Call(call.call_id, self._runtime._tool_name(call.tool), args, _EFFECT_NAMES[call.effect])

    def resume(self, result: Any) -> str:
        payload = _dumps(result).encode()
        self._status = lib.ld_exec_resume(self._ptr, payload, len(payload))
        return self.status

    def resume_error(self, message: str) -> str:
        self._status = lib.ld_exec_resume_error(self._ptr, message.encode())
        return self.status

    def result(self) -> Result:
        raw = lib.ld_exec_stats(self._ptr)
        stats = Stats(int(raw.vm_time_us), int(raw.peak_memory_bytes), int(raw.tool_calls))
        if self._status == 0:
            size = ffi.new("size_t*")
            out = lib.ld_exec_output(self._ptr, size)
            return Result(True, json.loads(ffi.unpack(out, size[0]).decode()), stats=stats)
        msg = ffi.new("const char**")
        kind = lib.ld_exec_error(self._ptr, msg)
        text = ffi.string(msg[0]).decode(errors="replace") if msg[0] != ffi.NULL else ""
        return Result(False, None, ERROR_KINDS[kind], text, stats)

    def close(self) -> None:
        if self._ptr is not None:
            lib.ld_exec_free(self._ptr)
            self._ptr = None

    def __enter__(self) -> "Execution":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()


class Runtime:
    def __init__(
        self,
        memory_mb: float = 16,
        cpu_time_ms: int = 1000,
        max_effects: dict[str, int] | None = None,
    ):
        limits = ffi.new("ld_limits*", lib.ld_limits_default())
        limits.memory_limit_bytes = int(memory_mb * 1024 * 1024)
        limits.cpu_time_ms = cpu_time_ms
        for effect, limit in (max_effects or {}).items():
            limits.max_effects[EFFECTS[effect]] = limit
        self._ptr = lib.ld_runtime_new(limits)
        if self._ptr == ffi.NULL:
            raise LunarDysonError("could not create runtime")
        self._tools: dict[str, Callable[[Any], Any] | None] = {}
        self._names: dict[int, str] = {}  # tool-name pointer -> str (pointers are stable once sealed)

    def _tool_name(self, ptr) -> str:
        key = int(ffi.cast("uintptr_t", ptr))
        name = self._names.get(key)
        if name is None:
            name = self._names[key] = ffi.string(ptr).decode()
        return name

    def tool(
        self,
        name: str,
        fn: Callable[[Any], Any] | None = None,
        *,
        signature: str | None = None,
        effect: str = "read",
        max_calls: int | None = None,
        max_response_bytes: int | None = None,
    ) -> "Runtime":
        rc = lib.ld_tool_register(
            self._ptr,
            name.encode(),
            signature.encode() if signature else ffi.NULL,
            EFFECTS[effect],
            UNLIMITED if max_calls is None else max_calls,
            max_response_bytes or 0,
        )
        if rc != 0:
            raise LunarDysonError(f"cannot register tool {name!r} (invalid name, duplicate, or runtime sealed)")
        self._tools[name] = fn
        return self

    def declare_types(self, declarations: str) -> "Runtime":
        if lib.ld_declare_types(self._ptr, declarations.encode()) != 0:
            raise LunarDysonError("cannot declare types after the runtime is sealed")
        return self

    def check(self, source: str, strict: bool = True) -> list[dict]:
        src = source.encode()
        raw = lib.ld_check(self._ptr, src, len(src), 1 if strict else 0)
        if raw == ffi.NULL:
            raise LunarDysonError("check failed")
        try:
            return json.loads(ffi.string(raw).decode())
        finally:
            lib.ld_free(raw)

    def start(self, source: str, input: Any = None, context: Any = None) -> Execution:
        src = source.encode()
        out = ffi.new("ld_exec**")
        status = lib.ld_exec_start(
            self._ptr,
            src,
            len(src),
            json.dumps(input if input is not None else {}).encode(),
            json.dumps(context if context is not None else {}).encode(),
            out,
        )
        if out[0] == ffi.NULL:
            raise LunarDysonError("could not start execution")
        return Execution(self, out[0], status)

    def execute(self, source: str, input: Any = None, context: Any = None) -> Result:
        with self.start(source, input, context) as ex:
            while ex._status == 1:  # pending_tool
                call = ex.pending()
                fn = self._tools.get(call.tool)
                if fn is None:
                    ex.resume_error(f"no Python handler registered for {call.tool}")
                    continue
                try:
                    value = fn(call.args)
                except Exception as e:  # surfaces inside the program as a tool error
                    ex.resume_error(f"{type(e).__name__}: {e}")
                    continue
                try:
                    ex.resume(value)
                except (TypeError, ValueError) as e:
                    ex.resume_error(f"tool result is not JSON-serializable: {e}")
            return ex.result()

    def close(self) -> None:
        if getattr(self, "_ptr", None) is not None:
            lib.ld_runtime_free(self._ptr)
            self._ptr = None

    def __enter__(self) -> "Runtime":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:
        self.close()
