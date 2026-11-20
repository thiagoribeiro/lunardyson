# Security model

LunarDyson runs **untrusted** Luau code inside the host process. What it
guarantees, what it does not, and how those guarantees are tested.

## Guarantees

| Threat | Control | Tested by |
|---|---|---|
| Host access (files, processes, env, network) | none of these exist in the VM; `os`, `io`, `debug`, `require`, `load*`, `getfenv`/`setfenv` removed | `sandbox_escape_globals_absent` |
| Loading hostile bytecode | the host passes source only, compiled by our own compiler; bytecode is never accepted | ABI design |
| Tampering with builtins or tools | `luaL_sandbox` makes builtins read-only; the `tools` tables are read-only with a locked metatable | `builtin_mutation`, `builtin_metatable_mutation`, `tools_*` |
| State leaking between executions | one `luaL_sandboxthread` per execution, with its own globals | `global_mutation_isolated_between_executions` |
| CPU exhaustion | VM-time deadline; the watchdog arms the interrupt and the error re-fires at every safepoint | `infinite_loop*`, `timeout_swallowed_by_pcall` |
| Memory exhaustion | budgeted allocator per runtime | `memory_bomb_*`, `huge_table`, `memory_swallowed_by_pcall` |
| Deep recursion | Luau call-depth limits | `recursive_bomb` |
| Calling capabilities that were not granted | unregistered tools do not exist (`unknown_tool`) | `unauthorized_*` |
| Abusing granted tools | per-tool `max_calls` / `max_response_bytes`; per-effect-class budgets | `tool_call_bomb`, `effect_budget_*`, `huge_tool_response` |
| Swallowing a violation with `pcall` | violations are sticky: the execution fails even if the program recovers | `*_swallowed_by_pcall` |

The full list is in `tests/conformance/cases.json`. It runs in every binding and
under ASan/UBSan (`LD_SANITIZE=ON`).

Two libFuzzer targets in `tests/fuzz/` complement the hand-written cases. CI runs them for
60 seconds on every pull request, and for longer every night.
- `fuzz_exec` runs arbitrary programs with arbitrary tool replies, and checks on every step:
  - only registered tools reach the host, each with its declared effect;
  - `call_id`s are sequential;
  - per-tool and per-effect budgets hold;
  - memory and VM time stay within budget;
  - every error has a known kind.
- `fuzz_check` feeds arbitrary source to the type checker.

A crash reproducer becomes a conformance case or a smoke test together with its fix.

## Non-guarantees

- **No process isolation.** A memory-safety bug in LunarDyson or in Luau
  compromises the host. For hostile workloads, run the runtime in a separate worker
  process or container.
- **Tools are the host's responsibility.** LunarDyson counts and limits tool calls,
  but what a tool does (SSRF checks, authorization, idempotency) is the host's job.
- **Timing side channels** are out of scope.
- **Out-of-range integer arguments to builtins are platform-dependent.** Library
  functions that take an integer (`string.sub`, `string.rep`, `string.char`,
  `table.insert`, …) convert the number with a plain `(int)` cast in Luau's
  `lua_tointegerx`. For a value outside the `int` range (e.g. `string.sub(s, 1e15)`),
  that cast is undefined behaviour in C++. It does not affect memory safety or the
  budgets, but x86-64 and arm64 can return different results. Upstream considers it
  known behaviour ([luau-lang/luau#3100](https://github.com/luau-lang/luau/issues/3100)),
  and LunarDyson does not fork Luau. The fuzz runs suppress this single report
  (`tests/fuzz/ubsan-suppressions.txt`) and treat any other UB as fatal.

## Thread safety of the timeout watchdog

The CPU-time deadline is enforced by a watchdog thread that sets the runtime's
`interrupt` pointer; the VM reads it at safepoints on the execution thread. Luau's
`VM/include/lua.h` sanctions this ("interrupt is safe to set from an arbitrary
thread"). The build runs clean under ThreadSanitizer (`-DLD_TSAN=ON`) with only that
single sanctioned read suppressed, documented in `tsan-suppressions.txt`. A runtime
otherwise runs one execution at a time and is not shared across threads.

## Updating Luau

Luau is pinned by the `extern/luau` submodule. Every bump must pass the full
conformance suite, the sanitizer build (ASan/UBSan and TSan) and the C smoke test.
