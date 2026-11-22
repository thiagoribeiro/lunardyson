"""Observers: event order, Prometheus series, OpenTelemetry spans, and zero impact on results."""

import pytest

import lunardyson as ld
from lunardyson.observe import outcome

TWO_CALLS = """
function run(input)
    local a = tools.crm.get({ id = "c1" })
    local b = tools.crm.get({ id = "c2" })
    return { n = 2 }
end
"""
LOOP = "function run() while true do end end"


class Recorder:
    def __init__(self):
        self.events = []

    def on_start(self, label):
        self.events.append(("start", label))
        return "state"

    def on_tool_call(self, state, label, call, duration_s, ok):
        assert state == "state" and duration_s >= 0
        self.events.append(("tool", call.call_id, call.tool, call.effect, ok))

    def on_finish(self, state, label, result, duration_s):
        assert state == "state" and duration_s >= 0
        self.events.append(("finish", outcome(result), result.error_kind))


def runtime(observer, **kw):
    rt = ld.Runtime(observer=observer, label="test", **kw)
    rt.tool("crm.get", lambda args: {"ok": True})
    return rt


def test_execute_reports_every_call_then_finishes_once():
    rec = Recorder()
    res = runtime(rec).execute(TWO_CALLS)
    assert res.ok
    assert rec.events == [
        ("start", "test"),
        ("tool", 1, "crm.get", "read", True),
        ("tool", 2, "crm.get", "read", True),
        ("finish", "ok", "none"),
    ]


def test_step_api_resume_error_and_repeated_result():
    rec = Recorder()
    rt = runtime(rec)
    with rt.start(TWO_CALLS) as ex:
        ex.pending()
        ex.pending()  # asking twice does not double-count the call
        ex.resume_error("boom")
        assert ex.status == "done" or ex.status == "error"
        ex.result()
        ex.result()
    tools = [e for e in rec.events if e[0] == "tool"]
    assert tools == [("tool", 1, "crm.get", "read", False)]
    assert sum(1 for e in rec.events if e[0] == "finish") == 1


def test_budget_violation_and_abandoned_execution():
    rec = Recorder()
    rt = runtime(rec, cpu_time_ms=50)
    assert not rt.execute(LOOP).ok
    assert rec.events[-1] == ("finish", "budget", "timeout")
    rec.events.clear()
    ex = rt.start(TWO_CALLS)
    ex.close()  # never resumed, never read
    assert rec.events == [("start", "test"), ("finish", "error", "abandoned")]


def test_failing_observer_never_breaks_execution():
    class Broken:
        def on_start(self, label):
            raise RuntimeError("x")

        def on_tool_call(self, *a):
            raise RuntimeError("x")

        def on_finish(self, *a):
            raise RuntimeError("x")

    rec = Recorder()
    res = runtime([Broken(), rec]).execute(TWO_CALLS)
    assert res.ok and res.output == {"n": 2}
    assert rec.events[-1] == ("finish", "ok", "none")


def test_prometheus_series():
    prom = pytest.importorskip("prometheus_client")
    from lunardyson.observe import PrometheusObserver

    reg = prom.CollectorRegistry()
    rt = runtime(PrometheusObserver(registry=reg), cpu_time_ms=50)
    rt.execute(TWO_CALLS)
    rt.execute(LOOP)
    PrometheusObserver(registry=reg)  # a second observer on the same registry shares the metrics

    def val(name, **labels):
        return reg.get_sample_value(name, labels)

    assert val("lunardyson_executions_total", runtime="test", outcome="ok", error_kind="none") == 1
    assert val("lunardyson_executions_total", runtime="test", outcome="budget", error_kind="timeout") == 1
    assert val("lunardyson_tool_calls_total", runtime="test", tool="crm.get", effect="read", ok="true") == 2
    assert val("lunardyson_tool_call_seconds_count", runtime="test", tool="crm.get") == 2
    assert val("lunardyson_budget_violations_total", runtime="test", kind="timeout") == 1
    assert val("lunardyson_execution_seconds_count", runtime="test") == 2
    assert val("lunardyson_peak_memory_bytes_count", runtime="test") == 2


def test_otel_spans():
    pytest.importorskip("opentelemetry.sdk")
    from opentelemetry.sdk.trace import TracerProvider
    from opentelemetry.sdk.trace.export import SimpleSpanProcessor
    from opentelemetry.sdk.trace.export.in_memory_span_exporter import InMemorySpanExporter

    from lunardyson.observe import OTelObserver

    exporter = InMemorySpanExporter()
    provider = TracerProvider()
    provider.add_span_processor(SimpleSpanProcessor(exporter))
    runtime(OTelObserver(tracer=provider.get_tracer("t"))).execute(TWO_CALLS)

    spans = {s.name: s for s in exporter.get_finished_spans()}
    root = spans["lunardyson.execute"]
    tools = [s for s in exporter.get_finished_spans() if s.name == "lunardyson.tool crm.get"]
    assert len(tools) == 2
    assert all(t.parent.span_id == root.context.span_id for t in tools)
    assert [t.attributes["lunardyson.call_id"] for t in tools] == [1, 2]
    assert root.attributes["lunardyson.outcome"] == "ok"
    assert root.attributes["lunardyson.tool_calls"] == 2
    assert all(root.start_time <= t.start_time <= t.end_time <= root.end_time for t in tools)
