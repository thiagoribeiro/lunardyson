"""cffi (ABI mode) declarations for lunardyson.h — no per-Python-version build needed."""

import os
from pathlib import Path

from cffi import FFI

ffi = FFI()
ffi.cdef(
    """
    typedef struct ld_runtime ld_runtime;
    typedef struct ld_exec ld_exec;
    typedef enum { LD_DONE = 0, LD_PENDING_TOOL = 1, LD_ERROR = 2 } ld_status;
    typedef enum {
        LD_ERR_NONE = 0, LD_ERR_SYNTAX, LD_ERR_TIMEOUT, LD_ERR_MEMORY, LD_ERR_TOOL_BUDGET,
        LD_ERR_EFFECT_BUDGET, LD_ERR_UNKNOWN_TOOL, LD_ERR_RUNTIME, LD_ERR_CONTRACT, LD_ERR_INVALID_ARG
    } ld_error_kind;
    typedef enum {
        LD_EFFECT_PURE = 0, LD_EFFECT_READ, LD_EFFECT_WRITE, LD_EFFECT_EXTERNAL_WRITE, LD_EFFECT_MONEY,
        LD_EFFECT_COUNT
    } ld_effect;
    typedef struct { size_t memory_limit_bytes; uint32_t cpu_time_ms; uint32_t max_effects[5]; } ld_limits;
    typedef struct {
        uint64_t call_id; const char* tool; const char* args_json; size_t args_len; ld_effect effect;
    } ld_call;
    typedef struct { uint64_t vm_time_us; size_t peak_memory_bytes; uint32_t tool_calls; } ld_stats;

    const char* ld_version(void);
    ld_limits ld_limits_default(void);
    ld_runtime* ld_runtime_new(const ld_limits* limits);
    void ld_runtime_free(ld_runtime* rt);
    int ld_tool_register(ld_runtime* rt, const char* name, const char* luau_signature, ld_effect effect,
                         uint32_t max_calls, size_t max_response_bytes);
    int ld_declare_types(ld_runtime* rt, const char* luau_type_declarations);
    ld_status ld_exec_start(ld_runtime* rt, const char* source, size_t source_len, const char* input_json,
                            const char* context_json, ld_exec** out);
    ld_status ld_exec_resume(ld_exec* ex, const char* result_json, size_t result_len);
    ld_status ld_exec_resume_error(ld_exec* ex, const char* message);
    int ld_exec_pending(const ld_exec* ex, ld_call* out);
    const char* ld_exec_output(const ld_exec* ex, size_t* len);
    ld_error_kind ld_exec_error(const ld_exec* ex, const char** message);
    ld_stats ld_exec_stats(const ld_exec* ex);
    void ld_exec_free(ld_exec* ex);
    char* ld_check(ld_runtime* rt, const char* source, size_t source_len, int strict);
    void ld_free(void* p);
    """
)

UNLIMITED = 0xFFFFFFFF


def _find_library() -> str:
    env = os.environ.get("LUNARDYSON_LIB")
    if env:
        return env
    here = Path(__file__).resolve().parent
    for candidate in (here / "liblunardyson.so", here.parents[2] / "build" / "liblunardyson.so"):
        if candidate.exists():
            return str(candidate)
    raise OSError("liblunardyson.so not found — build it or set LUNARDYSON_LIB")


lib = ffi.dlopen(_find_library())
