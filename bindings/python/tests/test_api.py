"""Step API, check(), lifecycle and concurrency behaviour of the Python binding."""

import threading

import pytest

import lunardyson as ld

TWO_CALLS = """
function run(input)
    local a = tools.crm.get({ id = input.first })
    local b = tools.crm.get({ id = input.second })
    return { names = { a.name, b.name } }
end
"""


def crm_runtime(**kwargs) -> ld.Runtime:
    rt = ld.Runtime(**kwargs)
    rt.tool("crm.get", lambda args: {"ok": True, "name": f"customer {args['id']}"})
    return rt


def test_version():
    assert ld.version() == "0.1.0"


def test_step_api_exposes_sequential_calls():
    rt = crm_runtime()
    with rt.start(TWO_CALLS, {"first": "c1", "second": "c2"}) as ex:
        assert ex.status == "pending_tool"
        call = ex.pending()
        assert (call.call_id, call.tool, call.args, call.effect) == (1, "crm.get", {"id": "c1"}, "read")
        assert ex.resume({"name": "Ana"}) == "pending_tool"
        assert ex.pending().call_id == 2
        assert ex.resume({"name": "Bruno"}) == "done"
        assert ex.result().output == {"names": ["Ana", "Bruno"]}


def test_replay_from_journal_is_deterministic():
    """A host can journal tool results and replay them on retry without re-running effects."""
    rt = crm_runtime()
    journal: dict[int, dict] = {}
    with rt.start(TWO_CALLS, {"first": "c1", "second": "c2"}) as ex:
        while ex.status == "pending_tool":
            call = ex.pending()
            journal[call.call_id] = {"name": f"live-{call.call_id}"}
            ex.resume(journal[call.call_id])
        first = ex.result().output
    with rt.start(TWO_CALLS, {"first": "c1", "second": "c2"}) as ex:
        while ex.status == "pending_tool":
            ex.resume(journal[ex.pending().call_id])
        assert ex.result().output == first


def test_interleaved_suspended_executions_on_one_runtime():
    rt = crm_runtime()
    a = rt.start(TWO_CALLS, {"first": "a1", "second": "a2"})
    b = rt.start(TWO_CALLS, {"first": "b1", "second": "b2"})
    b.resume({"name": "B1"})
    a.resume({"name": "A1"})
    a.resume({"name": "A2"})
    b.resume({"name": "B2"})
    assert a.result().output == {"names": ["A1", "A2"]}
    assert b.result().output == {"names": ["B1", "B2"]}
    a.close()
    b.close()


def test_tool_exception_is_catchable_by_the_program():
    rt = ld.Runtime()

    def boom(_args):
        raise ValueError("backend down")

    rt.tool("svc.call", boom)
    source = """
    function run()
        local ok, err = pcall(tools.svc.call, {})
        return { ok = ok, err = err }
    end"""
    result = rt.execute(source)
    assert result.ok
    assert result.output["ok"] is False
    assert "ValueError: backend down" in result.output["err"]


def test_resume_after_done_does_not_destroy_result():
    rt = ld.Runtime()
    with rt.start("function run() return { v = 1 } end") as ex:
        assert ex.status == "done"
        assert ex.resume({"x": 1}) == "error"  # misuse is reported...
        ex._status = 0
        assert ex.result().output == {"v": 1}  # ...but the execution is intact


def test_registration_closes_after_first_execution():
    rt = ld.Runtime()
    rt.execute("function run() return {} end")
    with pytest.raises(ld.LunarDysonError):
        rt.tool("late.tool", lambda a: a)


@pytest.mark.parametrize("name", ["noseparator", "a.b.c", "1ns.fn", "ns.fn-x"])
def test_invalid_tool_names_rejected(name):
    with pytest.raises(ld.LunarDysonError):
        ld.Runtime().tool(name, lambda a: a)


def test_many_executions_do_not_leak_into_the_memory_budget():
    rt = ld.Runtime(memory_mb=1)
    source = "function run() local t = {} for i = 1, 5000 do t[i] = tostring(i) end return { n = #t } end"
    for _ in range(300):
        result = rt.execute(source)
        assert result.ok, result.message


def test_parallel_runtimes_on_threads():
    source = "function run(input) local s = 0 for i = 1, 2e6 do s += i end return { s = s, id = input.id } end"
    results: dict[int, ld.Result] = {}

    def worker(i: int):
        with ld.Runtime(cpu_time_ms=5000) as rt:
            results[i] = rt.execute(source, {"id": i})

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert all(results[i].ok and results[i].output["id"] == i for i in range(4))


# ---------------------------------------------------------------------------
# check()
# ---------------------------------------------------------------------------

TYPES = """
type ToolError = { ok: false, error: string, message: string }
type Price = { ok: true, symbol: string, price: number }
type Input = { symbol: string }
type Context = { now: string }
type Output = { price: number } | { error: string, message: string }
"""


def typed_runtime() -> ld.Runtime:
    rt = ld.Runtime()
    rt.declare_types(TYPES)
    rt.tool(
        "market.get_price",
        lambda a: {"ok": True, "symbol": a["symbol"], "price": 1.0},
        signature="(args: { symbol: string }) -> Price | ToolError",
    )
    return rt


def errors(diags: list[dict]) -> list[dict]:
    return [d for d in diags if d["severity"] == "error"]


def test_check_accepts_refined_tagged_union():
    source = """
function run(input: Input, context: Context): Output
    local r = tools.market.get_price({ symbol = input.symbol })
    if not r.ok then
        return { error = r.error, message = r.message }
    end
    return { price = r.price }
end"""
    assert errors(typed_runtime().check(source)) == []


def test_check_flags_missing_ok_refinement_and_typos():
    source = """
function run(input: Input, context: Context): Output
    local r = tools.market.get_price({ symbol = input.symbol })
    return { price = r.prcie }
end"""
    diags = errors(typed_runtime().check(source))
    assert diags and all(d["kind"] == "TypeError" for d in diags)


def test_check_flags_hidden_globals_and_unknown_tools():
    rt = typed_runtime()
    diags = errors(rt.check("function run() return { t = os.time() } end"))
    assert any("os" in d["message"] for d in diags)
    diags = errors(rt.check("function run() return tools.payment.refund({}) end"))
    assert diags


def test_check_reports_syntax_errors():
    diags = errors(typed_runtime().check("function run( return {} end"))
    assert diags[0]["kind"] == "SyntaxError"


def test_check_bad_signature_reported_as_definition_error():
    rt = ld.Runtime()
    rt.tool("bad.sig", lambda a: a, signature="(args: { oops ) -> number")
    diags = errors(rt.check("function run() return {} end"))
    assert any(d["kind"] == "DefinitionError" for d in diags)


def test_non_json_tool_result_becomes_tool_error():
    import datetime

    rt = ld.Runtime()
    rt.tool("clock.now", lambda _args: {"at": datetime.datetime.now()})
    result = rt.execute("function run() local ok, err = pcall(tools.clock.now, {}) return { ok = ok, err = err } end")
    assert result.ok and result.output["ok"] is False
    assert "not JSON-serializable" in result.output["err"]
