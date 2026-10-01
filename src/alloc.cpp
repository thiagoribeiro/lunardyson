// Budgeted allocator handed to lua_newstate. Luau does not retry a failed
// allocation after a GC, so returning NULL raises LUA_ERRMEM immediately.
#include "internal.h"

#include <cstdlib>

void* ld_alloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
    auto* a = static_cast<AllocState*>(ud);
    size_t old = ptr ? osize : 0;

    if (nsize == 0)
    {
        if (ptr)
        {
            a->used -= old;
            free(ptr);
        }
        return nullptr;
    }

    if (nsize > old && a->used - old + nsize > a->limit)
    {
        a->exceeded = true;
        return nullptr;
    }

    void* p = realloc(ptr, nsize);
    if (!p)
        return nullptr;

    a->used = a->used - old + nsize;
    if (a->used > a->peak)
        a->peak = a->used;
    return p;
}
