#pragma once

#include "lunardyson.h"

#include "lua.h"

#include <chrono>
#include <string>
#include <vector>

struct Checker;

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
    std::string output;
    std::string input_json;
    std::string context_json;

    // Pending tool call
    bool pending = false;
    size_t pending_tool = 0;
    uint64_t pending_call_id = 0;
    std::string pending_args;

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

// alloc.cpp
void* ld_alloc(void* ud, void* ptr, size_t osize, size_t nsize);

// json.cpp — push decoded JSON onto the stack (returns false and sets *err on failure)
bool ld_json_push(lua_State* L, const char* json, size_t len, std::string* err);
// encode the value at `idx`; returns false and sets *err on failure
bool ld_json_encode(lua_State* L, int idx, std::string* out, std::string* err);

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
