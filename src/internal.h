#pragma once

#include "lunardyson.h"

#include "lua.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

struct Checker;

// Globals the program must not see: removed from the VM (runtime.cpp) and from the
// analyzer's global scope (check.cpp), so `check` and `execute` agree.
inline constexpr const char* kHiddenGlobals[] = {
    "getfenv", "setfenv", "loadstring", "load", "require", "newproxy", "gcinfo", "collectgarbage", "os", "debug",
    "coroutine", "io", "package", "dofile", "loadfile", "buffer", "vector",
};

// A JSON document written by yyjson (malloc'd), owned as-is instead of copied into a std::string.
struct JsonBuf
{
    struct Free
    {
        void operator()(char* p) const { free(p); }
    };
    std::unique_ptr<char, Free> data;
    size_t size = 0;
};

struct AllocState
{
    size_t used = 0;
    size_t limit = SIZE_MAX; // absolute cap on `used`; raised to baseline + budget once sealed
    size_t peak = 0;
    bool exceeded = false;
};

struct ToolDef
{
    std::string name; // "ns.fn"
    std::string ns;
    std::string fn;
    std::string signature;
    ld_effect effect;
    uint32_t max_calls;
    size_t max_response_bytes;
};

struct ld_runtime
{
    lua_State* L = nullptr;
    AllocState alloc;
    ld_limits limits{};
    std::vector<ToolDef> tools;
    std::string type_decls;
    bool sealed = false;
    size_t baseline = 0;
    ld_exec* running = nullptr; // execution currently inside lua_resume
    bool live_exec = false; // an execution exists (started, not yet freed) — at most one at a time
    uint64_t slice_generation = 0; // identifies the current VM slice to the watchdog
    Checker* checker = nullptr;
};

struct ld_exec
{
    enum class Phase { Load, Run, Finished };

    ld_runtime* rt = nullptr;
    lua_State* T = nullptr;
    int thread_ref = LUA_NOREF;
    Phase phase = Phase::Load;

    ld_status status = LD_ERROR;
    ld_error_kind error = LD_ERR_NONE;
    std::string message;
    JsonBuf output;
    std::string input_json;
    std::string context_json;

    // Pending tool call
    bool pending = false;
    size_t pending_tool = 0;
    uint64_t pending_call_id = 0;
    JsonBuf pending_args;

    // Budget accounting
    uint64_t next_call_id = 1;
    uint32_t total_calls = 0;
    std::vector<uint32_t> calls_per_tool;
    uint32_t effects_used[LD_EFFECT_COUNT] = {};

    // Violations are sticky: a program that swallows the error with pcall still fails.
    ld_error_kind sticky = LD_ERR_NONE;
    std::string sticky_message;

    // VM time: accumulated only while this execution is inside lua_resume
    uint64_t vm_ns = 0;
    std::chrono::steady_clock::time_point slice_start;
    size_t peak_bytes = 0;
};

// The execution currently inside lua_resume on L's runtime, if any.
inline ld_exec* ld_running(lua_State* L)
{
    auto* rt = static_cast<ld_runtime*>(lua_callbacks(L)->userdata);
    return rt ? rt->running : nullptr;
}

// alloc.cpp
void* ld_alloc(void* ud, void* ptr, size_t osize, size_t nsize);

// json.cpp — push decoded JSON onto the stack (returns false and sets *err on failure)
bool ld_json_push(lua_State* L, const char* json, size_t len, std::string* err);
// encode the value at `idx`; returns false and sets *err on failure
bool ld_json_encode(lua_State* L, int idx, JsonBuf* out, std::string* err);
// True when s[0..len) is well-formed UTF-8 (the common case: no copy needed).
bool ld_utf8_valid(const char* s, size_t len);
// Copy of s with every byte sequence that is not well-formed UTF-8 replaced by U+FFFD.
// JSON output must be valid UTF-8 (yyjson refuses to write anything else), but Luau
// strings, and analyzer messages quoting the source, are arbitrary bytes.
std::string ld_utf8_sanitize(const char* s, size_t len);

// tools.cpp
void ld_tools_install(ld_runtime* rt); // builds the read-only `tools` global (before sandboxing)

// runtime.cpp — closes registration: installs tools, sandboxes globals, sets the memory budget
void ld_runtime_seal(ld_runtime* rt);

// exec.cpp
void ld_set_sticky(ld_exec* ex, ld_error_kind kind, const std::string& message);
void ld_interrupt(lua_State* L, int gc);

// watchdog.cpp — arms the interrupt only once a slice passes its deadline
void ld_watchdog_arm(ld_runtime* rt, uint64_t generation, std::chrono::steady_clock::time_point deadline);
void ld_watchdog_disarm(ld_runtime* rt, uint64_t generation);

// check.cpp
void ld_checker_free(Checker* c);
