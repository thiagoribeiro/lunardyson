# LunarDyson for Python

**Let the model write the program. Decide what it can touch.**

LunarDyson runs untrusted [Luau](https://luau.org) programs that orchestrate tools. The host
registers the tools, each with its own budget. The program runs in a sandboxed Luau VM, and
the only way it can reach the outside world is through those tools. Before it runs, it is
type-checked against the tool signatures. While it runs, everything it does is metered.

```
pip install lunardyson
```

Wheels are published for Linux x86_64 and aarch64 (glibc 2.28 or newer) and work on every
Python from 3.10: the binding uses cffi in ABI mode, so there is no per-version build.

## A quick look

```python
import lunardyson as ld

def crm_get(args):
    return {"ok": True, "name": "Ada"}

rt = ld.Runtime(memory_mb=16, cpu_time_ms=1000, max_effects={"money": 0})
rt.declare_types("type Customer = { ok: true, name: string } | { ok: false, error: string }")
rt.tool("crm.get", crm_get, signature="(args: { id: string }) -> Customer",
        effect="read", max_calls=20)

source = """
function run(input: { id: string }): { greeting: string?, error: string? }
    local c = tools.crm.get({ id = input.id })
    if not c.ok then
        return { error = c.error }
    end
    return { greeting = "Hello, " .. c.name }
end
"""

errors = [d for d in rt.check(source) if d["severity"] == "error"]   # [] when type-safe
result = rt.execute(source, {"id": "c1"}, {"now": "2026-09-30T12:00:00Z"})
print(result.ok, result.output)   # True {'greeting': 'Hello, Ada'}
```

- **Default deny:** no `os`, `io`, `debug`, `require`, `load*` or `coroutine`. A tool the host
  did not register does not exist in the program's environment.
- **Budgets:** memory, VM time, calls and response bytes per tool, and calls per effect class
  (`pure`, `read`, `write`, `external_write`, `money`). A violation fails the execution even
  inside `pcall`.
- **Step API:** `start()`/`resume()` stop at every tool call and hand it to the host, so sync,
  async and remote tools all use the same loop.
- **Threads:** a runtime runs one execution at a time; the binding releases the GIL inside C
  calls, so a pool of runtimes runs programs in parallel.

## Observability

```
pip install "lunardyson[prometheus]"   # Prometheus counters and histograms
pip install "lunardyson[otel]"         # OpenTelemetry spans (API only)
```

```python
from lunardyson.observe import OTelObserver, PrometheusObserver

rt = ld.Runtime(observer=[PrometheusObserver(), OTelObserver()], label="scripts")
```

## More

- [Website](https://thiagoribeiro.github.io/lunardyson/)
- [Repository, C ABI and Go binding](https://github.com/thiagoribeiro/lunardyson)
- [Security model](https://github.com/thiagoribeiro/lunardyson/blob/main/SECURITY.md)

Apache-2.0.
