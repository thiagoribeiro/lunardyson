"""Per-execution overhead: LunarDyson vs lupa (the engine RiteSmith uses today).

    LUNARDYSON_LIB=build/liblunardyson.so python bench/overhead.py

Rows:
  lunardyson (reused runtime)  — the intended usage: one sealed runtime, many executions
  lunardyson (fresh runtime)   — new runtime per execution, apples-to-apples with production lupa
  lupa (fresh runtime)         — mirrors ritesmith/runtime/sandbox.py: new LuaRuntime, globals
                                 removed, tools injected, run() called, result converted
  ritesmith run_in_sandbox     — the real production path, when `ritesmith` is importable
"""

import json
import os
import platform
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bindings" / "python"))

import lunardyson as ld  # noqa: E402

ORDERS = {
    "o1": {"id": "o1", "status": "paid", "items": [{"sku": "A", "qty": 2}, {"sku": "B", "qty": 1}]},
    "o2": {"id": "o2", "status": "pending", "items": [{"sku": "A", "qty": 1}]},
    "o3": {"id": "o3", "status": "paid", "items": [{"sku": "A", "qty": 1}, {"sku": "C", "qty": 5}]},
    "o4": {"id": "o4", "status": "paid", "items": [{"sku": "B", "qty": 3}]},
}
STOCK = {"A": 10, "B": 3, "C": 2}


def orders_get(args):
    order = ORDERS.get(args["id"])
    return {"ok": True, "order": order} if order else {"ok": False, "error": "not_found", "message": "no order"}


TOOLS = {
    "market.get_price": lambda args: {"ok": True, "symbol": args["symbol"], "price": 534000.12},
    "orders.get": orders_get,
    "inventory.check": lambda args: {"ok": True, "available": STOCK.get(args["sku"], 0)},
    "shipping.create": lambda args: {"ok": True, "tracking": "TRK-" + args["order_id"]},
}

T3_BODY = """
local function firstShortage(order)
    for _, item in ipairs(order.items) do
        local stock = tools.inventory.check({ sku = item.sku })
        if not stock.ok or stock.available < item.qty then
            return item.sku
        end
    end
    return nil
end

function run(input, context)
    local shipped, blocked = {}, {}
    for _, id in ipairs(input.order_ids) do
        local r = tools.orders.get({ id = id })
        if not r.ok then
            table.insert(blocked, { order_id = id, reason = "not_found" })
        elseif r.order.status ~= "paid" then
            table.insert(blocked, { order_id = id, reason = "not_paid" })
        else
            local sku = firstShortage(r.order)
            if sku then
                table.insert(blocked, { order_id = id, reason = "out_of_stock:" .. sku })
            else
                local s = tools.shipping.create({ order_id = id })
                table.insert(shipped, { order_id = id, tracking = s.tracking })
            end
        end
    end
    return { shipped = shipped, blocked = blocked }
end
"""

# Sources are written in the common Lua 5.x / Luau subset so both engines run the same text.
SCENARIOS = [
    ("empty run", "function run(input, context) return { ok = true } end", {}, 3000),
    (
        "1 tool call",
        "function run(input, context) local p = tools.market.get_price({ symbol = input.symbol }) "
        "return { price = p.price } end",
        {"symbol": "BTC"},
        3000,
    ),
    ("T3 order fulfillment (13 tool calls)", T3_BODY, {"order_ids": ["o1", "o2", "o3", "o9", "o4"]}, 1000),
    (
        "CPU: 200k loop iterations",
        "function run(input) local s = 0 for i = 1, 200000 do s = s + i % 7 end return { s = s } end",
        {},
        100,
    ),
]


def timed_interleaved(fns: dict, n: int) -> dict:
    """Round-robin the engines so machine noise hits all of them equally; returns medians in µs."""
    for fn in fns.values():  # warm-up
        for _ in range(min(50, n)):
            fn()
    samples = {name: [] for name in fns}
    for _ in range(n):
        for name, fn in fns.items():
            t = time.perf_counter()
            fn()
            samples[name].append(time.perf_counter() - t)
    return {name: statistics.median(xs) * 1e6 for name, xs in samples.items()}


def new_ld_runtime() -> ld.Runtime:
    rt = ld.Runtime(cpu_time_ms=5000)
    for name, fn in TOOLS.items():
        rt.tool(name, fn)
    return rt


def bench_lunardyson_reused(source, input_):
    rt = new_ld_runtime()

    def once():
        r = rt.execute(source, input_)
        assert r.ok, r.message

    return once


def bench_lunardyson_fresh(source, input_):
    def once():
        with new_ld_runtime() as rt:
            r = rt.execute(source, input_)
            assert r.ok, r.message

    return once


def bench_lupa(source, input_):
    from lupa import LuaRuntime

    forbidden = ["io", "os", "debug", "load", "loadfile", "dofile", "require", "package", "rawget", "rawset",
                 "rawequal", "collectgarbage", "newproxy"]

    def to_py(v):
        if hasattr(v, "items"):
            d = dict(v.items())
            if d and all(isinstance(k, int) for k in d) and sorted(d) == list(range(1, len(d) + 1)):
                return [to_py(d[i]) for i in range(1, len(d) + 1)]
            return {str(k): to_py(x) for k, x in d.items()}
        return v

    def once():
        lua = LuaRuntime(unpack_returned_tuples=False)
        g = lua.globals()
        for name in forbidden:
            g[name] = None
        tools = lua.table()
        for name, fn in TOOLS.items():
            ns, fname = name.split(".")
            if tools[ns] is None:
                tools[ns] = lua.table()
            tools[ns][fname] = (lambda f: lambda args: lua.table_from(f(to_py(args)), recursive=True))(fn)
        g["tools"] = tools
        lua.execute(source)
        out = to_py(g["run"](lua.table_from(input_, recursive=True), lua.table()))
        assert out is not None

    return once


def bench_ritesmith(source, input_):
    try:
        from ritesmith.runtime.sandbox import run_in_sandbox
    except Exception:
        return None
    if "tools." in source:
        return None  # production exposes host functions differently; only pure scenarios compare

    def once():
        out, err, _ = run_in_sandbox(source, input_, {}, profile="transform_only", timeout_ms=5000)
        assert err is None, err

    return once


def main() -> None:
    engines = [
        ("lunardyson (reused runtime)", bench_lunardyson_reused),
        ("lunardyson (fresh runtime)", bench_lunardyson_fresh),
        ("lupa (fresh runtime)", bench_lupa),
        ("ritesmith run_in_sandbox", bench_ritesmith),
    ]
    rows = []
    for scenario, source, input_, n in SCENARIOS:
        fns = {engine: fn for engine, factory in engines if (fn := factory(source, input_)) is not None}
        medians = timed_interleaved(fns, n)
        base = medians.get("lupa (fresh runtime)")
        for engine, us in medians.items():
            ratio = f"{us / base:5.2f}× lupa" if base else ""
            rows.append({"scenario": scenario, "engine": engine, "median_us": round(us, 1)})
            print(f"{scenario:40} {engine:30} {us:>10.1f} µs  {ratio}", flush=True)

    meta = {"machine": platform.machine(), "python": platform.python_version(), "lunardyson": ld.version()}
    out = Path(os.environ.get("LD_BENCH_OUT", "bench/results.json"))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"meta": meta, "rows": rows}, indent=2))
    print(f"\n{meta} → {out}")


if __name__ == "__main__":
    main()
