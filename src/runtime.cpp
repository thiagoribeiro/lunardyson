// ld_runtime: one sandboxed lua_State with a budgeted allocator, a fixed set of
// safe libraries and the registered tools. Registration closes ("seals") at the
// first execution or check; after that the globals are frozen by luaL_sandbox.
#include "internal.h"

#include "lualib.h"

#include <cstdlib>
#include <cstring>

#ifndef LD_VERSION
#define LD_VERSION "0.0.0"
#endif

namespace
{

// Safe libraries only: no os, debug, coroutine, buffer, vector.
const luaL_Reg kLibs[] = {
    {"", luaopen_base},
    {LUA_TABLIBNAME, luaopen_table},
    {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {LUA_BITLIBNAME, luaopen_bit32},
    {nullptr, nullptr},
};

// Removed from the base library (environment access, GC control, host I/O).
const char* const kHiddenGlobals[] = {
    "getfenv", "setfenv", "loadstring", "load", "require", "newproxy", "gcinfo", "collectgarbage", "os", "debug",
    "coroutine", "io", "package", "dofile", "loadfile",
};

int print_noop(lua_State*)
{
    return 0; // the host's stdout is not the program's to write to
}

bool valid_identifier(const std::string& s)
{
    if (s.empty() || !(isalpha((unsigned char)s[0]) || s[0] == '_'))
        return false;
    for (char c : s)
        if (!(isalnum((unsigned char)c) || c == '_'))
            return false;
    return true;
}

} // namespace

extern "C" {

LD_API const char* ld_version(void)
{
    return LD_VERSION;
}

LD_API ld_limits ld_limits_default(void)
{
    ld_limits l{};
    l.memory_limit_bytes = 16u * 1024 * 1024;
    l.cpu_time_ms = 1000;
    for (uint32_t& e : l.max_effects)
        e = LD_UNLIMITED;
    return l;
}

LD_API ld_runtime* ld_runtime_new(const ld_limits* limits)
{
    auto* rt = new ld_runtime();
    rt->limits = limits ? *limits : ld_limits_default();

    rt->L = lua_newstate(ld_alloc, &rt->alloc);
    if (!rt->L)
    {
        delete rt;
        return nullptr;
    }
    lua_callbacks(rt->L)->userdata = rt;
    // interrupt stays unset; the watchdog arms it only when a deadline passes

    try
    {
        for (const luaL_Reg* lib = kLibs; lib->func; ++lib)
        {
            lua_pushcfunction(rt->L, lib->func, nullptr);
            lua_pushstring(rt->L, lib->name);
            lua_call(rt->L, 1, 0);
        }
        for (const char* name : kHiddenGlobals)
        {
            lua_pushnil(rt->L);
            lua_setglobal(rt->L, name);
        }
        lua_pushcfunction(rt->L, print_noop, "print");
        lua_setglobal(rt->L, "print");
    }
    catch (...)
    {
        lua_close(rt->L);
        delete rt;
        return nullptr;
    }
    return rt;
}

LD_API void ld_runtime_free(ld_runtime* rt)
{
    if (!rt)
        return;
    ld_checker_free(rt->checker);
    if (rt->L)
        lua_close(rt->L);
    delete rt;
}

LD_API int ld_tool_register(ld_runtime* rt, const char* name, const char* luau_signature, ld_effect effect,
                            uint32_t max_calls, size_t max_response_bytes)
{
    if (!rt || rt->sealed || !name || effect < 0 || effect >= LD_EFFECT_COUNT)
        return -1;
    std::string full = name;
    size_t dot = full.find('.');
    if (dot == std::string::npos || full.find('.', dot + 1) != std::string::npos)
        return -1; // exactly "namespace.function"
    ToolDef t;
    t.name = full;
    t.ns = full.substr(0, dot);
    t.fn = full.substr(dot + 1);
    if (!valid_identifier(t.ns) || !valid_identifier(t.fn))
        return -1;
    for (const ToolDef& other : rt->tools)
        if (other.name == t.name)
            return -1;
    t.signature = luau_signature && *luau_signature ? luau_signature : "(args: any) -> any";
    t.effect = effect;
    t.max_calls = max_calls;
    t.max_response_bytes = max_response_bytes ? max_response_bytes : SIZE_MAX;
    rt->tools.push_back(std::move(t));
    return 0;
}

LD_API int ld_declare_types(ld_runtime* rt, const char* decls)
{
    if (!rt || rt->sealed || !decls)
        return -1;
    rt->type_decls += decls;
    rt->type_decls += "\n";
    return 0;
}

LD_API void ld_free(void* p)
{
    free(p);
}

} // extern "C"

void ld_runtime_seal(ld_runtime* rt)
{
    if (rt->sealed)
        return;
    rt->sealed = true;
    ld_tools_install(rt);
    luaL_sandbox(rt->L);
    lua_gc(rt->L, LUA_GCCOLLECT, 0);
    rt->baseline = rt->alloc.used;
    rt->alloc.limit = rt->baseline + rt->limits.memory_limit_bytes;
}
