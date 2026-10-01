# Spike v0.1 — results (2026-09-30)

Goal: decide whether a generic Luau runtime (C ABI + bindings) is viable as the
execution engine for RiteSmith and other hosts.

## Cut criteria

| Criterion | Result |
|---|---|
| Every conformance case yields the expected `error_kind`, in Python **and** Go | ✅ 32/32 in Python, 32/32 in Go; also passes on arm64 (R2D2, production image) |
| `infinite_loop` stops at `cpu_time_ms` ±10%, no stuck thread | ✅ 200.1 ms for a 200 ms budget; also under `pcall` and when the timeout is swallowed |
| The 25 RiteSmith benchmark references run through the lib and pass `ld_check(strict)` | ✅ 25/25 run, 25/25 strict-clean |
| Overhead per execution ≤ lupa on R2D2 (median) | ⚠️ **partial**, see below |
| `lunardyson.h` fits on ~1 page; tool bridge < ~300 lines of C++ | ✅ 106 lines / 18 functions; `tools.cpp` 119 lines |
| Memory safety | ✅ 100 tests under ASan+UBSan; C smoke under LeakSanitizer (2000 executions, error paths, abandoned execution, 50 checks): no leaks |

## Overhead vs lupa (median; engines interleaved per iteration)

x86_64 dev machine (Python 3.14):

| scenario | lunardyson (reused runtime) | lupa (fresh runtime, like production) |
|---|---|---|
| empty run | **0.43×** | 1.00× |
| 1 tool call | **0.60×** | 1.00× |
| T3 order fulfillment (13 tool calls) | **0.98×** | 1.00× |
| CPU: 200k loop iterations | 1.01× | 1.00× |

R2D2, Orange Pi Zero 3, Cortex-A53, inside `ritesmith:latest` (Python 3.12, load ~5):

| scenario | lunardyson (reused) | lupa (fresh) | RiteSmith `run_in_sandbox` (production path) |
|---|---|---|---|
| empty run | **0.96×** (1.02 ms) | 1.00× (1.07 ms) | 1.99× (2.12 ms) |
| 1 tool call | 1.12× | 1.00× | — (different host-function ABI) |
| T3 (13 tool calls) | 1.30× (3.3 ms) | 1.00× (2.5 ms) | — |
| CPU: 200k loop iterations | 1.26× | 1.00× | 1.11× |

The A53 is ~20× slower than the dev machine across the board (the C `start` path
takes 97 µs vs 4 µs; measured with CPU time, not wall time).

Where time goes on R2D2:
- **Tool calls:** ~50 µs extra per call, spent in the Python binding's JSON
  encode/decode at the ABI boundary (the v0 design choice). This is negligible next
  to real tool latency (HTTP/DB: tens to hundreds of ms).
- **Pure CPU:** Luau's interpreter with double-precision `%` loses to Lua 5.5's
  native integers on arm64. The lever here is Luau's native codegen
  (`Luau.CodeGen` supports arm64), which is on the roadmap.
- **Fixed per-execution cost:** `ld_exec_free` runs a full GC (48 µs on the A53),
  because Luau does not collect before failing an allocation.

Compared with the path RiteSmith actually runs today (`run_in_sandbox`, which
includes the thread-pool hop), LunarDyson is ~2× faster per execution.

## Design changes made during the spike

- **Watchdog deadlines.** The first version ran an interrupt callback at every
  safepoint, costing ~30% on CPU-bound code. Now one watchdog thread arms the
  interrupt only once a slice passes its deadline, and it wakes about once per
  budget window. Normal execution pays nothing.
- **Exported type aliases.** Only `export type` aliases from a definition file
  reach the global scope, so `ld_declare_types` exports plain `type X = …` for the
  host.
- **Non-destructive API misuse.** A `resume` with no pending call reports
  `LD_ERROR` without mutating the execution.

## Next

1. RiteSmith backend `runtime=luau` behind `run_in_sandbox`, plus the generation
   policy (types in the prompt, strict gate with nonstrict fallback).
2. Python binding tool path: fewer allocations, faster JSON (e.g. an optional
   `orjson`), or a zero-copy value API.
3. Optional native codegen for CPU-bound programs.
4. A GC policy that avoids a full collection on every execution while keeping
   the memory budget honest.
5. Isolation tiers: a worker process pool and `lunardyson-server`.
6. CI: wheels for x86_64 and aarch64, sanitizers, fuzzing.
