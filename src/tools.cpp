// The `tools` global: read-only nested tables of C closures. Calling a tool
// checks its budgets, serializes the argument table and yields the execution
// back to the host (LD_PENDING_TOOL). Nothing that was not registered exists;
// touching an unknown name fails the execution with LD_ERR_UNKNOWN_TOOL.
#include "internal.h"

#include "lualib.h"

#include <cstring>

namespace
{

ld_exec* current_exec(lua_State* L)
{
    auto* rt = static_cast<ld_runtime*>(lua_callbacks(L)->userdata);
    return rt ? rt->running : nullptr;
}

[[noreturn]] void fail(lua_State* L, ld_exec* ex, ld_error_kind kind, const std::string& msg)
{
    if (ex)
        ld_set_sticky(ex, kind, msg);
    luaL_error(L, "%s", msg.c_str());
}

int tool_call(lua_State* L)
{
    ld_exec* ex = current_exec(L);
    size_t index = size_t(lua_tonumber(L, lua_upvalueindex(1)));
    if (!ex)
        luaL_error(L, "tools can only be called during an execution");

    ld_runtime* rt = ex->rt;
    const ToolDef& tool = rt->tools[index];

    if (tool.max_calls != LD_UNLIMITED && ex->calls_per_tool[index] >= tool.max_calls)
        fail(L, ex, LD_ERR_TOOL_BUDGET,
             "tool call budget exceeded: " + tool.name + " (max " + std::to_string(tool.max_calls) + ")");

    uint32_t effect_limit = rt->limits.max_effects[tool.effect];
    if (effect_limit != LD_UNLIMITED && ex->effects_used[tool.effect] >= effect_limit)
        fail(L, ex, LD_ERR_EFFECT_BUDGET,
             "effect budget exceeded by " + tool.name + " (max " + std::to_string(effect_limit) + ")");

    std::string args, err;
    if (lua_gettop(L) == 0 || lua_isnil(L, 1))
        args = "{}";
    else if (!ld_json_encode(L, 1, &args, &err))
        luaL_error(L, "tools.%s: argument is not JSON-serializable: %s", tool.name.c_str(), err.c_str());

    ex->calls_per_tool[index]++;
    ex->effects_used[tool.effect]++;
    ex->total_calls++;
    ex->pending = true;
    ex->pending_tool = index;
    ex->pending_call_id = ex->next_call_id++;
    ex->pending_args = std::move(args);
    return lua_yield(L, 0);
}

int unknown_tool(lua_State* L)
{
    const char* prefix = lua_tostring(L, lua_upvalueindex(1));
    const char* key = lua_isstring(L, 2) ? lua_tostring(L, 2) : "?";
    std::string name = std::string(prefix) + key;
    fail(L, current_exec(L), LD_ERR_UNKNOWN_TOOL, "unknown tool: " + name + " (not registered)");
}

// Locks the table at `idx`: unknown keys raise, metatable is hidden and frozen.
void seal_table(lua_State* L, int idx, const std::string& prefix)
{
    idx = lua_absindex(L, idx);
    lua_createtable(L, 0, 2);
    lua_pushstring(L, prefix.c_str());
    lua_pushcclosure(L, unknown_tool, "unknown_tool", 1);
    lua_rawsetfield(L, -2, "__index");
    lua_pushliteral(L, "locked");
    lua_rawsetfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, true);
    lua_setmetatable(L, idx);
    lua_setreadonly(L, idx, true);
}

} // namespace

void ld_tools_install(ld_runtime* rt)
{
    lua_State* L = rt->L;
    lua_newtable(L); // tools

    for (size_t i = 0; i < rt->tools.size(); ++i)
    {
        const ToolDef& t = rt->tools[i];
        lua_rawgetfield(L, -1, t.ns.c_str());
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_rawsetfield(L, -3, t.ns.c_str());
        }
        lua_pushnumber(L, double(i));
        lua_pushcclosure(L, tool_call, t.name.c_str(), 1);
        lua_rawsetfield(L, -2, t.fn.c_str());
        lua_pop(L, 1);
    }

    // Seal every namespace table, then the root.
    lua_pushnil(L);
    while (lua_next(L, -2) != 0)
    {
        std::string ns = lua_tostring(L, -2);
        seal_table(L, -1, "tools." + ns + ".");
        lua_pop(L, 1);
    }
    seal_table(L, -1, "tools.");
    lua_setglobal(L, "tools");
}
