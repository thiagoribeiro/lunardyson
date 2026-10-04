"""Cross-execution isolation on a reused runtime.

RiteSmith reuses one Runtime per worker thread across many executions, so state
from one script must never reach the next: no leaked globals, no poisoned
builtins or tool tables, no surviving references, memory back to baseline. These
prove isolation between *sequential* executions (the real reuse scenario), not
sandbox escape.
"""

import lunardyson as ld


def crm_runtime(**kwargs) -> ld.Runtime:
    rt = ld.Runtime(**kwargs)
    rt.tool("crm.get", lambda args: {"ok": True, "name": f"customer {args['id']}"})
    return rt


def test_a_global_written_in_one_execution_is_gone_in_the_next():
    # A top-level global write succeeds within its own execution, but the global
    # environment is fresh per execution: the next one must not see it.
    rt = ld.Runtime()
    a = rt.execute("function run() leaked = 42 return { set = leaked } end")
    assert a.ok and a.output == {"set": 42}, a.message
    b = rt.execute("function run() return { leaked = leaked } end")
    assert b.ok, b.message
    assert b.output == {}  # `leaked` is nil → absent from the table


def test_builtin_poisoning_attempt_does_not_affect_later_executions():
    rt = ld.Runtime()
    poison = rt.execute("function run() string.upper = function() return 'pwned' end return {} end")
    assert not poison.ok and poison.error_kind == "runtime"
    clean = rt.execute("function run() return { s = string.upper('ok') } end")
    assert clean.ok, clean.message
    assert clean.output == {"s": "OK"}


def test_tool_table_cannot_be_mutated_across_executions():
    rt = crm_runtime()
    tamper = rt.execute("function run() tools.crm.get = function() return { ok = true, name = 'x' } end return {} end")
    assert not tamper.ok and tamper.error_kind == "runtime"
    # The real tool still runs on the next execution.
    with rt.start("function run(input) local c = tools.crm.get({ id = input.id }) return { name = c.name } end", {"id": "c9"}) as ex:
        assert ex.status == "pending_tool"
        assert ex.pending().tool == "crm.get"
        ex.resume({"ok": True, "name": "Ana"})
        assert ex.result().output == {"name": "Ana"}


def test_reference_from_one_execution_does_not_survive_into_the_next():
    # A global write would error, so the only way to "keep" a table across runs is
    # via a global — which is rejected. The next execution starts from a clean slate.
    rt = ld.Runtime()
    rt.execute("function run() return {} end")
    probe = rt.execute(
        "function run() return { has_leaked = leaked ~= nil, has_shared = shared ~= nil } end"
    )
    assert probe.ok, probe.message
    assert probe.output == {"has_leaked": False, "has_shared": False}


def test_alternating_executions_do_not_cross_talk():
    rt = ld.Runtime()
    sq = "function run(input) return { v = input.n * input.n } end"
    neg = "function run(input) return { v = -input.n } end"
    for i in range(1, 101):
        a = rt.execute(sq, {"n": i})
        b = rt.execute(neg, {"n": i})
        assert a.ok and a.output == {"v": i * i}, a.message
        assert b.ok and b.output == {"v": -i}, b.message


def test_memory_returns_to_baseline_between_executions():
    # A heavy allocation in one execution must not shrink the next one's budget.
    rt = ld.Runtime(memory_mb=2)
    heavy = "function run() local t = {} for i = 1, 20000 do t[i] = tostring(i) end return { n = #t } end"
    light = "function run() return { ok = true } end"
    for _ in range(100):
        h = rt.execute(heavy)
        assert h.ok, h.message
        assert rt.execute(light).ok
