// JSON <-> Luau values, straight between yyjson and the Lua stack.
//
// Encoding rules (shared with the RiteSmith benchmark harness): a table whose
// keys are exactly 1..n is an array, anything else is an object with string
// keys; integral numbers below 1e15 are written as integers; NaN/inf become
// null; an empty table encodes as {}.
#include "internal.h"

#include "yyjson.h"

#include <cmath>
#include <cstdio>
#include <memory>

namespace
{

constexpr int kMaxDepth = 40;

struct DocFree
{
    void operator()(yyjson_doc* d) const { yyjson_doc_free(d); }
    void operator()(yyjson_mut_doc* d) const { yyjson_mut_doc_free(d); }
};

bool push_value(lua_State* L, yyjson_val* v, int depth, std::string* err)
{
    if (depth > kMaxDepth)
    {
        *err = "JSON nesting too deep";
        return false;
    }
    if (!lua_checkstack(L, 3))
    {
        *err = "Lua stack exhausted";
        return false;
    }

    switch (yyjson_get_type(v))
    {
    case YYJSON_TYPE_NULL:
        lua_pushnil(L);
        return true;
    case YYJSON_TYPE_BOOL:
        lua_pushboolean(L, yyjson_get_bool(v));
        return true;
    case YYJSON_TYPE_NUM:
        lua_pushnumber(L, yyjson_get_num(v));
        return true;
    case YYJSON_TYPE_STR:
        lua_pushlstring(L, yyjson_get_str(v), yyjson_get_len(v));
        return true;
    case YYJSON_TYPE_ARR:
    {
        lua_createtable(L, int(yyjson_arr_size(v)), 0);
        size_t idx, max;
        yyjson_val* item;
        yyjson_arr_foreach(v, idx, max, item)
        {
            if (!push_value(L, item, depth + 1, err))
                return false;
            lua_rawseti(L, -2, int(idx + 1));
        }
        return true;
    }
    case YYJSON_TYPE_OBJ:
    {
        lua_createtable(L, 0, int(yyjson_obj_size(v)));
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(v, idx, max, key, val)
        {
            lua_pushlstring(L, yyjson_get_str(key), yyjson_get_len(key));
            if (!push_value(L, val, depth + 1, err))
                return false;
            lua_rawset(L, -3);
        }
        return true;
    }
    default:
        *err = "unsupported JSON value";
        return false;
    }
}

yyjson_mut_val* encode_value(yyjson_mut_doc* doc, lua_State* L, int idx, int depth, std::string* err);

yyjson_mut_val* encode_table(yyjson_mut_doc* doc, lua_State* L, int idx, int depth, std::string* err)
{
    size_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0)
    {
        ++count;
        lua_pop(L, 1);
    }

    size_t n = lua_objlen(L, idx);
    if (count > 0 && n == count)
    {
        yyjson_mut_val* arr = yyjson_mut_arr(doc);
        for (size_t i = 1; i <= n; ++i)
        {
            lua_rawgeti(L, idx, int(i));
            yyjson_mut_val* item = encode_value(doc, L, lua_gettop(L), depth + 1, err);
            lua_pop(L, 1);
            if (!item)
                return nullptr;
            yyjson_mut_arr_append(arr, item);
        }
        return arr;
    }

    yyjson_mut_val* obj = yyjson_mut_obj(doc);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0)
    {
        yyjson_mut_val* key;
        int kt = lua_type(L, -2);
        if (kt == LUA_TSTRING)
        {
            size_t len;
            const char* s = lua_tolstring(L, -2, &len);
            const std::string clean = ld_utf8_sanitize(s, len);
            key = yyjson_mut_strncpy(doc, clean.data(), clean.size());
        }
        else if (kt == LUA_TNUMBER)
        {
            char buf[64];
            double d = lua_tonumber(L, -2);
            if (std::floor(d) == d && std::fabs(d) < 1e15)
                snprintf(buf, sizeof(buf), "%lld", (long long)d);
            else
                snprintf(buf, sizeof(buf), "%.14g", d);
            key = yyjson_mut_strcpy(doc, buf);
        }
        else
        {
            *err = std::string("table key of type ") + lua_typename(L, kt) + " is not JSON-serializable";
            lua_pop(L, 2);
            return nullptr;
        }
        yyjson_mut_val* val = encode_value(doc, L, lua_gettop(L), depth + 1, err);
        if (!val)
        {
            lua_pop(L, 2);
            return nullptr;
        }
        yyjson_mut_obj_add(obj, key, val);
        lua_pop(L, 1);
    }
    return obj;
}

yyjson_mut_val* encode_value(yyjson_mut_doc* doc, lua_State* L, int idx, int depth, std::string* err)
{
    if (depth > kMaxDepth)
    {
        *err = "value nesting too deep (cycle?)";
        return nullptr;
    }
    if (!lua_checkstack(L, 4))
    {
        *err = "Lua stack exhausted";
        return nullptr;
    }

    switch (lua_type(L, idx))
    {
    case LUA_TNIL:
        return yyjson_mut_null(doc);
    case LUA_TBOOLEAN:
        return yyjson_mut_bool(doc, lua_toboolean(L, idx));
    case LUA_TNUMBER:
    {
        double d = lua_tonumber(L, idx);
        if (!std::isfinite(d))
            return yyjson_mut_null(doc);
        if (std::floor(d) == d && std::fabs(d) < 1e15)
            return yyjson_mut_sint(doc, (int64_t)d);
        return yyjson_mut_real(doc, d);
    }
    case LUA_TSTRING:
    {
        size_t len;
        const char* s = lua_tolstring(L, idx, &len);
        const std::string clean = ld_utf8_sanitize(s, len);
        return yyjson_mut_strncpy(doc, clean.data(), clean.size());
    }
    case LUA_TTABLE:
        return encode_table(doc, L, idx, depth, err);
    default:
        *err = std::string("value of type ") + lua_typename(L, lua_type(L, idx)) + " is not JSON-serializable";
        return nullptr;
    }
}

} // namespace

bool ld_json_push(lua_State* L, const char* json, size_t len, std::string* err)
{
    yyjson_read_err rerr;
    std::unique_ptr<yyjson_doc, DocFree> doc(yyjson_read_opts(const_cast<char*>(json), len, 0, nullptr, &rerr));
    if (!doc)
    {
        *err = std::string("invalid JSON: ") + rerr.msg + " at byte " + std::to_string(rerr.pos);
        return false;
    }
    int top = lua_gettop(L);
    if (!push_value(L, yyjson_doc_get_root(doc.get()), 0, err))
    {
        lua_settop(L, top);
        return false;
    }
    return true;
}

bool ld_json_encode(lua_State* L, int idx, std::string* out, std::string* err)
{
    if (idx < 0 && idx > LUA_REGISTRYINDEX)
        idx = lua_gettop(L) + idx + 1;

    std::unique_ptr<yyjson_mut_doc, DocFree> doc(yyjson_mut_doc_new(nullptr));
    if (!doc)
    {
        *err = "out of memory";
        return false;
    }
    int top = lua_gettop(L);
    yyjson_mut_val* root = encode_value(doc.get(), L, idx, 0, err);
    lua_settop(L, top);
    if (!root)
        return false;
    yyjson_mut_doc_set_root(doc.get(), root);

    size_t len = 0;
    char* s = yyjson_mut_write(doc.get(), 0, &len);
    if (!s)
    {
        *err = "JSON serialization failed";
        return false;
    }
    out->assign(s, len);
    free(s);
    return true;
}

std::string ld_utf8_sanitize(const char* s, size_t len)
{
    static constexpr char kReplacement[] = "\xEF\xBF\xBD"; // U+FFFD
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    std::string out;
    out.reserve(len);
    size_t i = 0;
    while (i < len)
    {
        const unsigned char c = p[i];
        if (c < 0x80)
        {
            out.push_back(char(c));
            ++i;
            continue;
        }
        size_t n;     // continuation bytes
        uint32_t cp;  // code point being decoded
        if (c >= 0xC2 && c <= 0xDF)
            n = 1, cp = c & 0x1F;
        else if (c >= 0xE0 && c <= 0xEF)
            n = 2, cp = c & 0x0F;
        else if (c >= 0xF0 && c <= 0xF4)
            n = 3, cp = c & 0x07;
        else
            n = 0, cp = 0; // stray continuation byte, 0xC0/0xC1 or 0xF5..0xFF

        bool ok = n > 0 && i + n < len;
        for (size_t k = 1; ok && k <= n; ++k)
        {
            if ((p[i + k] & 0xC0) != 0x80)
                ok = false;
            else
                cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        // Overlong forms, UTF-16 surrogates and code points past U+10FFFF are not UTF-8.
        if (ok && ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
                   (cp >= 0xD800 && cp <= 0xDFFF)))
            ok = false;

        if (ok)
        {
            out.append(s + i, n + 1);
            i += n + 1;
        }
        else
        {
            out += kReplacement;
            ++i;
        }
    }
    return out;
}
