"""Observability for the Python binding: metrics and traces from the host process.

LunarDyson runs in the host process, so everything an execution does is visible
from here. Pass an observer to the runtime::

    from lunardyson.observe import PrometheusObserver, OTelObserver

    rt = ld.Runtime(observer=PrometheusObserver(), label="scripts")
    rt = ld.Runtime(observer=[PrometheusObserver(), OTelObserver()], label="scripts")

Events come from the step API, so they cover ``execute()`` and hosts that drive
``start()``/``resume()`` themselves (async or remote tools) alike:

- ``on_start(label)`` when an execution starts; its return value is the
  execution's state, handed back to the other two methods;
- ``on_tool_call(state, label, call, duration_s, ok)`` when the host resumes a
  pending call (``ok`` is False for ``resume_error``); the duration is the time
  between ``pending()`` and the resume, i.e. the host's tool latency;
- ``on_finish(state, label, result, duration_s)`` once, the first time
  ``result()`` is read.

``prometheus_client`` and ``opentelemetry-api`` are optional
(``pip install lunardyson[prometheus]`` / ``lunardyson[otel]``). Observer errors
are swallowed: observability must never fail an execution.
"""

from __future__ import annotations

import time
from typing import TYPE_CHECKING, Any, Protocol

if TYPE_CHECKING:
    from lunardyson import Call, Result

BUDGET_KINDS = frozenset({"timeout", "memory", "tool_budget", "effect_budget"})

_SECONDS_BUCKETS = (0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10)
_BYTES_BUCKETS = tuple(64 * 1024 * 4**i for i in range(7))  # 64 KiB … 256 MiB


class Observer(Protocol):
    def on_start(self, label: str) -> Any: ...

    def on_tool_call(self, state: Any, label: str, call: Call, duration_s: float, ok: bool) -> None: ...

    def on_finish(self, state: Any, label: str, result: Result, duration_s: float) -> None: ...


def outcome(result: Result) -> str:
    """'ok', 'budget' (timeout, memory, tool or effect budget) or 'error'."""
    if result.ok:
        return "ok"
    return "budget" if result.error_kind in BUDGET_KINDS else "error"


class _Fanout:
    """Several observers behind one; a failing observer does not affect the others."""

    def __init__(self, observers: list[Observer]):
        self.observers = observers

    def on_start(self, label):
        states = []
        for o in self.observers:
            try:
                states.append(o.on_start(label))
            except Exception:
                states.append(None)
        return states

    def on_tool_call(self, state, label, call, duration_s, ok):
        for o, s in zip(self.observers, state, strict=True):
            try:
                o.on_tool_call(s, label, call, duration_s, ok)
            except Exception:
                pass

    def on_finish(self, state, label, result, duration_s):
        for o, s in zip(self.observers, state, strict=True):
            try:
                o.on_finish(s, label, result, duration_s)
            except Exception:
                pass


def combine(observer: Observer | list[Observer] | None) -> _Fanout | None:
    if observer is None:
        return None
    return _Fanout(list(observer) if isinstance(observer, (list, tuple)) else [observer])


class PrometheusObserver:
    """Prometheus metrics, labelled by runtime ``label`` (never by call id).

    - ``<ns>_executions_total{runtime,outcome,error_kind}``
    - ``<ns>_execution_seconds{runtime}`` — wall time from start to result
    - ``<ns>_vm_seconds{runtime}`` — time inside the VM
    - ``<ns>_peak_memory_bytes{runtime}``
    - ``<ns>_tool_calls_total{runtime,tool,effect,ok}``
    - ``<ns>_tool_call_seconds{runtime,tool}`` — host tool latency
    - ``<ns>_budget_violations_total{runtime,kind}``

    Metrics are created once per (registry, namespace); building several observers
    on the same registry shares them instead of failing on duplicate registration.
    """

    _shared: dict[tuple[int, str], dict] = {}

    def __init__(self, registry=None, namespace: str = "lunardyson"):
        from prometheus_client import REGISTRY, Counter, Histogram

        registry = registry if registry is not None else REGISTRY
        key = (id(registry), namespace)
        m = self._shared.get(key)
        if m is None:
            kw = {"namespace": namespace, "registry": registry}
            m = self._shared[key] = {
                "executions": Counter("executions", "Executions by outcome",
                                      ["runtime", "outcome", "error_kind"], **kw),
                "execution_seconds": Histogram("execution_seconds", "Wall time from start to result",
                                               ["runtime"], buckets=_SECONDS_BUCKETS, **kw),
                "vm_seconds": Histogram("vm_seconds", "Time spent inside the VM",
                                        ["runtime"], buckets=_SECONDS_BUCKETS, **kw),
                "peak_memory": Histogram("peak_memory_bytes", "Peak VM heap per execution",
                                         ["runtime"], buckets=_BYTES_BUCKETS, **kw),
                "tool_calls": Counter("tool_calls", "Tool calls resumed by the host",
                                      ["runtime", "tool", "effect", "ok"], **kw),
                "tool_seconds": Histogram("tool_call_seconds", "Host tool latency",
                                          ["runtime", "tool"], buckets=_SECONDS_BUCKETS, **kw),
                "violations": Counter("budget_violations", "Executions stopped by a budget",
                                      ["runtime", "kind"], **kw),
            }
        self.m = m

    def on_start(self, label):
        return None

    def on_tool_call(self, state, label, call, duration_s, ok):
        self.m["tool_calls"].labels(label, call.tool, call.effect, "true" if ok else "false").inc()
        self.m["tool_seconds"].labels(label, call.tool).observe(duration_s)

    def on_finish(self, state, label, result, duration_s):
        self.m["executions"].labels(label, outcome(result), result.error_kind).inc()
        self.m["execution_seconds"].labels(label).observe(duration_s)
        self.m["vm_seconds"].labels(label).observe(result.stats.vm_time_us / 1e6)
        self.m["peak_memory"].labels(label).observe(result.stats.peak_memory_bytes)
        if result.error_kind in BUDGET_KINDS:
            self.m["violations"].labels(label, result.error_kind).inc()


class OTelObserver:
    """OpenTelemetry spans: ``lunardyson.execute`` per execution and a child
    ``lunardyson.tool <name>`` per tool call (call id, effect, ok as attributes).

    Uses only ``opentelemetry-api``; the host configures the SDK and exporter. The
    execution span's parent is whatever span is current when the execution starts.
    Tool spans are recorded with the measured start/end times; spans the tool
    handler itself creates are not nested under them.
    """

    def __init__(self, tracer=None):
        from opentelemetry import trace

        self._trace = trace
        self.tracer = tracer if tracer is not None else trace.get_tracer("lunardyson")

    def on_start(self, label):
        span = self.tracer.start_span("lunardyson.execute", attributes={"lunardyson.runtime": label})
        return span

    def on_tool_call(self, state, label, call, duration_s, ok):
        end = time.time_ns()
        ctx = self._trace.set_span_in_context(state)
        span = self.tracer.start_span(
            f"lunardyson.tool {call.tool}",
            context=ctx,
            start_time=end - int(duration_s * 1e9),
            attributes={
                "lunardyson.tool": call.tool,
                "lunardyson.call_id": call.call_id,
                "lunardyson.effect": call.effect,
                "lunardyson.ok": ok,
            },
        )
        if not ok:
            span.set_status(self._trace.Status(self._trace.StatusCode.ERROR))
        span.end(end_time=end)

    def on_finish(self, state, label, result, duration_s):
        state.set_attributes({
            "lunardyson.outcome": outcome(result),
            "lunardyson.error_kind": result.error_kind,
            "lunardyson.vm_time_us": result.stats.vm_time_us,
            "lunardyson.peak_memory_bytes": result.stats.peak_memory_bytes,
            "lunardyson.tool_calls": result.stats.tool_calls,
        })
        if not result.ok:
            state.set_status(self._trace.Status(self._trace.StatusCode.ERROR, result.error_kind))
        state.end()
