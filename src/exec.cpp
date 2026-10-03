// ld_exec: one program run on its own sandboxed thread, driven step by step.
//
//   Load phase  — the compiled chunk runs (defines `run`, may call tools)
//   Run phase   — run(input, context) is called; its table result is the output
//
// Each step is a lua_resume. A tool call yields (LD_PENDING_TOOL) and the host
// resumes with the JSON result. Budget violations are sticky: once one fires,
// the execution fails with that kind even if the program catches the error.
#include "internal.h"

#include "luacode.h"
#include "lualib.h"

#include <cstdlib>
#include <cstring>

using Clock = std::chrono::steady_clock;

namespace
{

const char* const kContractMessage = "Script must define a 'run(input, context)' function (global, not local)";

ld_status finish_error(ld_exec* ex, ld_error_kind kind, std::string message)
{
    ex->phase = ld_exec::Phase::Finished;
    ex->status = LD_ERROR;
    ex->error = kind;
    ex->message = std::move(message);
    ex->pending = false;
    return ex->status;
}

ld_status finish_sticky(ld_exec* ex)
{
    return finish_error(ex, ex->sticky, ex->sticky_message);
}

std::string error_text(lua_State* T)
{
    if (lua_gettop(T) == 0)
        return "unknown error";
    if (const char* s = lua_tostring(T, -1))
        return s;
    return std::string("error object is a ") + luaL_typename(T, -1);
}

// Push a JSON document onto T, converting Luau errors (e.g. memory) into false.
bool push_json(ld_exec* ex, const std::string& json, std::string* err)
{
    try
    {
        return ld_json_push(ex->T, json.data(), json.size(), err);
    }
    catch (...)
    {
        *err = "memory budget exceeded while loading JSON";
        ld_set_sticky(ex, LD_ERR_MEMORY, *err);
        return false;
    }
}

ld_status step(ld_exec* ex, int nargs, bool raise);

// Raise `message` inside the program at the pending yield point.
ld_status resume_with_error(ld_exec* ex, const std::string& message)
{
    try
    {
        lua_pushstring(ex->T, message.c_str());
    }
    catch (...)
    {
        ld_set_sticky(ex, LD_ERR_MEMORY, "memory budget exceeded");
        return finish_sticky(ex);
    }
    return step(ex, 0, true);
}

// Host API misuse (e.g. resume with nothing pending): reported, never mutates the execution.
ld_status misuse(ld_exec*)
{
    return LD_ERROR;
}

ld_status after_load(ld_exec* ex)
{
    lua_State* T = ex->T;
    lua_settop(T, 0);
    lua_getglobal(T, "run");
    if (!lua_isfunction(T, -1))
        return finish_error(ex, LD_ERR_CONTRACT, kContractMessage);

    std::string err;
    if (!push_json(ex, ex->input_json, &err) || !push_json(ex, ex->context_json, &err))
        return ex->sticky != LD_ERR_NONE ? finish_sticky(ex) : finish_error(ex, LD_ERR_INVALID_ARG, err);

    ex->phase = ld_exec::Phase::Run;
    return step(ex, 2, false);
}

ld_status after_run(ld_exec* ex)
{
    lua_State* T = ex->T;
    if (lua_gettop(T) == 0 || !lua_istable(T, 1))
        return finish_error(ex, LD_ERR_CONTRACT, "run() must return a table");

    std::string out, err;
    if (!ld_json_encode(T, 1, &out, &err))
        return finish_error(ex, LD_ERR_CONTRACT, "run() result is not JSON-serializable: " + err);

    ex->phase = ld_exec::Phase::Finished;
    ex->status = LD_DONE;
    ex->output = std::move(out);
    return LD_DONE;
}

ld_status step(ld_exec* ex, int nargs, bool raise)
{
    ld_runtime* rt = ex->rt;
    rt->running = ex;
    rt->alloc.exceeded = false;
    rt->alloc.peak = rt->alloc.used;
    ex->slice_start = Clock::now();

    uint64_t budget_ns = uint64_t(rt->limits.cpu_time_ms) * 1000000ull;
    uint64_t remaining_ns = budget_ns > ex->vm_ns ? budget_ns - ex->vm_ns : 0;
    uint64_t generation = ++rt->slice_generation;
    // Already out of budget from earlier slices: mark the timeout here (only this thread
    // touches sticky) instead of writing the interrupt pointer ourselves — the watchdog is
    // the sole writer of ->interrupt. We still arm it (deadline in the past) as a backstop.
    if (remaining_ns == 0)
        ld_set_sticky(ex, LD_ERR_TIMEOUT,
                      "execution exceeded " + std::to_string(rt->limits.cpu_time_ms) + " ms");
    ld_watchdog_arm(rt, generation, ex->slice_start + std::chrono::nanoseconds(remaining_ns));

    int status = raise ? lua_resumeerror(ex->T, nullptr) : lua_resume(ex->T, nullptr, nargs);

    ld_watchdog_disarm(rt, generation);
    ex->vm_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - ex->slice_start).count());
    rt->running = nullptr;
    if (rt->alloc.peak > rt->baseline && rt->alloc.peak - rt->baseline > ex->peak_bytes)
        ex->peak_bytes = rt->alloc.peak - rt->baseline;
    if (rt->alloc.exceeded || status == LUA_ERRMEM)
        ld_set_sticky(ex, LD_ERR_MEMORY,
                      "memory budget exceeded (" + std::to_string(rt->limits.memory_limit_bytes) + " bytes)");

    if (status == LUA_YIELD)
    {
        if (ex->sticky != LD_ERR_NONE)
            return finish_sticky(ex);
        if (!ex->pending)
            return finish_error(ex, LD_ERR_RUNTIME, "program yielded outside a tool call");
        ex->status = LD_PENDING_TOOL;
        return LD_PENDING_TOOL;
    }
    if (status != LUA_OK)
    {
        if (ex->sticky != LD_ERR_NONE)
            return finish_sticky(ex);
        return finish_error(ex, LD_ERR_RUNTIME, error_text(ex->T));
    }
    if (ex->sticky != LD_ERR_NONE)
        return finish_sticky(ex);

    return ex->phase == ld_exec::Phase::Load ? after_load(ex) : after_run(ex);
}

} // namespace

void ld_set_sticky(ld_exec* ex, ld_error_kind kind, const std::string& message)
{
    if (ex->sticky == LD_ERR_NONE)
    {
        ex->sticky = kind;
        ex->sticky_message = message;
    }
}

void ld_interrupt(lua_State* L, int gc)
{
    if (gc >= 0)
        return; // never raise from inside the garbage collector

    auto* rt = static_cast<ld_runtime*>(lua_callbacks(L)->userdata);
    ld_exec* ex = rt ? rt->running : nullptr;
    if (!ex)
        return;

    if (ex->sticky == LD_ERR_TIMEOUT)
    {
        // Keep failing at every safepoint so pcall cannot swallow the timeout for long.
        lua_rawcheckstack(L, 1);
        luaL_error(L, "%s", ex->sticky_message.c_str());
    }

    uint64_t elapsed =
        ex->vm_ns + uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - ex->slice_start).count());
    if (elapsed > uint64_t(rt->limits.cpu_time_ms) * 1000000ull)
    {
        ld_set_sticky(ex, LD_ERR_TIMEOUT, "execution exceeded " + std::to_string(rt->limits.cpu_time_ms) + " ms");
        lua_rawcheckstack(L, 1);
        luaL_error(L, "%s", ex->sticky_message.c_str());
    }
    // Armed a hair early (clock skew between threads): keep checking until the budget is really spent.
}

extern "C" {

LD_API ld_status ld_exec_start(ld_runtime* rt, const char* source, size_t source_len, const char* input_json,
                               const char* context_json, ld_exec** out)
{
    if (!out)
        return LD_ERROR;
    *out = nullptr;
    if (!rt || !source)
        return LD_ERROR;
    if (rt->live_exec)
        return LD_ERROR; // one in-flight execution per runtime — use a pool for concurrency

    auto* ex = new ld_exec();
    *out = ex;
    rt->live_exec = true;
    ex->rt = rt;
    ex->input_json = input_json && *input_json ? input_json : "{}";
    ex->context_json = context_json && *context_json ? context_json : "{}";

    try
    {
        ld_runtime_seal(rt);
        ex->calls_per_tool.assign(rt->tools.size(), 0);

        ex->T = lua_newthread(rt->L);
        ex->thread_ref = lua_ref(rt->L, -1);
        lua_pop(rt->L, 1);
        luaL_sandboxthread(ex->T);
    }
    catch (...)
    {
        return finish_error(ex, LD_ERR_MEMORY, "memory budget exceeded while creating the execution");
    }

    // Always compile from source: bytecode from outside is never loaded (the VM does not validate it).
    lua_CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    size_t bytecode_len = 0;
    char* bytecode = luau_compile(source, source_len, &opts, &bytecode_len);
    if (!bytecode)
        return finish_error(ex, LD_ERR_MEMORY, "compiler out of memory");

    int load_status;
    try
    {
        load_status = luau_load(ex->T, "=program", bytecode, bytecode_len, 0);
    }
    catch (...)
    {
        free(bytecode);
        return finish_error(ex, LD_ERR_MEMORY, "memory budget exceeded while loading the program");
    }
    free(bytecode);
    if (load_status != 0)
        return finish_error(ex, LD_ERR_SYNTAX, error_text(ex->T));

    ex->phase = ld_exec::Phase::Load;
    return step(ex, 0, false);
}

LD_API ld_status ld_exec_resume(ld_exec* ex, const char* result_json, size_t result_len)
{
    if (!ex || ex->status != LD_PENDING_TOOL || !ex->pending)
        return misuse(ex);

    const ToolDef& tool = ex->rt->tools[ex->pending_tool];
    ex->pending = false;

    if (result_len > tool.max_response_bytes)
    {
        std::string msg = "tool response too large: " + tool.name + " returned " + std::to_string(result_len) +
                          " bytes (max " + std::to_string(tool.max_response_bytes) + ")";
        ld_set_sticky(ex, LD_ERR_TOOL_BUDGET, msg);
        return resume_with_error(ex, msg);
    }

    std::string err;
    if (!result_json || !push_json(ex, std::string(result_json, result_len), &err))
    {
        if (ex->sticky != LD_ERR_NONE)
            return finish_sticky(ex);
        return finish_error(ex, LD_ERR_INVALID_ARG, "tool " + tool.name + " result: " + (result_json ? err : "null"));
    }
    return step(ex, 1, false);
}

LD_API ld_status ld_exec_resume_error(ld_exec* ex, const char* message)
{
    if (!ex || ex->status != LD_PENDING_TOOL || !ex->pending)
        return misuse(ex);
    ex->pending = false;
    const ToolDef& tool = ex->rt->tools[ex->pending_tool];
    return resume_with_error(ex, "tools." + tool.name + ": " + (message ? message : "tool failed"));
}

LD_API int ld_exec_pending(const ld_exec* ex, ld_call* out)
{
    if (!ex || !out || ex->status != LD_PENDING_TOOL || !ex->pending)
        return 0;
    const ToolDef& tool = ex->rt->tools[ex->pending_tool];
    out->call_id = ex->pending_call_id;
    out->tool = tool.name.c_str();
    out->args_json = ex->pending_args.data();
    out->args_len = ex->pending_args.size();
    out->effect = tool.effect;
    return 1;
}

LD_API const char* ld_exec_output(const ld_exec* ex, size_t* len)
{
    if (!ex || ex->status != LD_DONE)
    {
        if (len)
            *len = 0;
        return nullptr;
    }
    if (len)
        *len = ex->output.size();
    return ex->output.c_str();
}

LD_API ld_error_kind ld_exec_error(const ld_exec* ex, const char** message)
{
    if (!ex)
    {
        if (message)
            *message = "null execution";
        return LD_ERR_INVALID_ARG;
    }
    if (message)
        *message = ex->message.c_str();
    return ex->status == LD_ERROR ? ex->error : LD_ERR_NONE;
}

LD_API ld_stats ld_exec_stats(const ld_exec* ex)
{
    ld_stats s{};
    if (ex)
    {
        s.vm_time_us = ex->vm_ns / 1000;
        s.peak_memory_bytes = ex->peak_bytes;
        s.tool_calls = ex->total_calls;
    }
    return s;
}

LD_API void ld_exec_free(ld_exec* ex)
{
    if (!ex)
        return;
    if (ex->rt)
        ex->rt->live_exec = false; // the runtime can start a new execution again
    if (ex->thread_ref != LUA_NOREF && ex->rt && ex->rt->L)
    {
        lua_unref(ex->rt->L, ex->thread_ref);
        // Luau does not collect before failing an allocation, so reclaim now to
        // keep the next execution's memory budget honest.
        lua_gc(ex->rt->L, LUA_GCCOLLECT, 0);
    }
    delete ex;
}

} // extern "C"
