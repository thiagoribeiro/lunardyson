/* LunarDyson — execution runtime for untrusted Luau programs that orchestrate tools.
 *
 * Execution is step-based: a program runs until it calls a tool, then yields
 * LD_PENDING_TOOL back to the host. The host runs the tool however it likes
 * (sync, async, remote) and resumes with the JSON result. No callbacks cross
 * the ABI boundary.
 *
 * A runtime is NOT thread-safe: use it from one thread at a time (several
 * suspended executions may coexist on it). For parallelism, use a pool of
 * runtimes. All JSON strings are UTF-8 and need not be NUL-terminated when a
 * length is given.
 */
#ifndef LUNARDYSON_H
#define LUNARDYSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define LD_API __declspec(dllexport)
#else
#define LD_API __attribute__((visibility("default")))
#endif

#define LD_UNLIMITED 0xFFFFFFFFu

typedef struct ld_runtime ld_runtime;
typedef struct ld_exec ld_exec;

typedef enum { LD_DONE = 0, LD_PENDING_TOOL = 1, LD_ERROR = 2 } ld_status;

typedef enum {
    LD_ERR_NONE = 0,
    LD_ERR_SYNTAX,        /* program does not compile */
    LD_ERR_TIMEOUT,       /* VM time budget exceeded */
    LD_ERR_MEMORY,        /* memory budget exceeded */
    LD_ERR_TOOL_BUDGET,   /* per-tool max_calls or max_response_bytes exceeded */
    LD_ERR_EFFECT_BUDGET, /* per-effect-class budget exceeded */
    LD_ERR_UNKNOWN_TOOL,  /* program referenced a tool that was not registered */
    LD_ERR_RUNTIME,       /* any other runtime error raised by the program */
    LD_ERR_CONTRACT,      /* no global run(input, context), or its result is not a plain table */
    LD_ERR_INVALID_ARG    /* API misuse by the host (bad JSON, resume without pending call, ...) */
} ld_error_kind;

typedef enum {
    LD_EFFECT_PURE = 0,
    LD_EFFECT_READ,
    LD_EFFECT_WRITE,
    LD_EFFECT_EXTERNAL_WRITE,
    LD_EFFECT_MONEY,
    LD_EFFECT_COUNT
} ld_effect;

typedef struct {
    size_t memory_limit_bytes;             /* heap ceiling above the sealed baseline, for the single
                                              in-flight execution (one execution per runtime) */
    uint32_t cpu_time_ms;                  /* VM time per execution; tool time is not counted */
    uint32_t max_effects[LD_EFFECT_COUNT]; /* calls per effect class per execution; LD_UNLIMITED */
} ld_limits;

typedef struct {
    uint64_t call_id;      /* sequential per execution, starting at 1 — use it for journaling/replay */
    const char* tool;      /* "namespace.function" */
    const char* args_json; /* valid until the next ld_exec_* call on this execution */
    size_t args_len;
    ld_effect effect;
} ld_call;

typedef struct {
    uint64_t vm_time_us;
    size_t peak_memory_bytes; /* peak heap above baseline while this execution was active */
    uint32_t tool_calls;
} ld_stats;

LD_API const char* ld_version(void);
LD_API ld_limits ld_limits_default(void); /* 16 MiB, 1000 ms, every effect unlimited */

/* Runtime: tools and type declarations must be registered before the first execution. */
LD_API ld_runtime* ld_runtime_new(const ld_limits* limits);
LD_API void ld_runtime_free(ld_runtime* rt);
LD_API int ld_tool_register(ld_runtime* rt, const char* name, const char* luau_signature, ld_effect effect,
                            uint32_t max_calls, size_t max_response_bytes); /* 0 on success */
LD_API int ld_declare_types(ld_runtime* rt, const char* luau_type_declarations); /* used by ld_check only */

/* Execution. A runtime runs at most one execution at a time: ld_exec_start returns LD_ERROR
 * (with *out left NULL) if an execution started on this runtime has not been freed yet. For
 * concurrency, use a pool of runtimes. */
LD_API ld_status ld_exec_start(ld_runtime* rt, const char* source, size_t source_len, const char* input_json,
                               const char* context_json, ld_exec** out);
LD_API ld_status ld_exec_resume(ld_exec* ex, const char* result_json, size_t result_len);
LD_API ld_status ld_exec_resume_error(ld_exec* ex, const char* message); /* raise a tool error in the program */
LD_API int ld_exec_pending(const ld_exec* ex, ld_call* out); /* 1 if a tool call is pending */
LD_API const char* ld_exec_output(const ld_exec* ex, size_t* len); /* JSON result when LD_DONE */
LD_API ld_error_kind ld_exec_error(const ld_exec* ex, const char** message);
LD_API ld_stats ld_exec_stats(const ld_exec* ex);
LD_API void ld_exec_free(ld_exec* ex);

/* Static analysis: returns a JSON array [{"line","column","kind","message","severity"}]; free with ld_free. */
LD_API char* ld_check(ld_runtime* rt, const char* source, size_t source_len, int strict);
LD_API void ld_free(void* p);

#ifdef __cplusplus
}
#endif
#endif
