// libFuzzer target: ld_check (Luau.Analysis) on arbitrary source. The type checker
// reads LLM-written code before anything runs, so it must never crash or leak, and it
// must always answer with a JSON array of diagnostics.
//
// Input layout: first byte selects strict (odd) or nonstrict (even); the rest is source.

#include "lunardyson.h"

#include <cstdio>
#include <cstdlib>

namespace
{

ld_runtime* runtime()
{
    static ld_runtime* rt = [] {
        ld_runtime* r = ld_runtime_new(nullptr);
        if (!r)
            std::abort();
        ld_declare_types(r, "type Customer = { ok: true, name: string } | { ok: false, error: string }\n"
                            "type Input = { id: string, n: number? }\n"
                            "type Output = { [string]: any }\n");
        ld_tool_register(r, "crm.get", "(args: { id: string }) -> Customer", LD_EFFECT_READ, 8, 4096);
        ld_tool_register(r, "mail.send", "(args: { to: string, body: string }) -> { ok: boolean }",
                         LD_EFFECT_EXTERNAL_WRITE, 2, 4096);
        return r;
    }();
    return rt;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0)
        return 0;
    const int strict = data[0] & 1;
    char* diagnostics = ld_check(runtime(), reinterpret_cast<const char*>(data + 1), size - 1, strict);
    if (!diagnostics)
    {
        std::fprintf(stderr, "INVARIANT VIOLATED: ld_check returned NULL\n");
        std::abort();
    }
    if (diagnostics[0] != '[')
    {
        std::fprintf(stderr, "INVARIANT VIOLATED: ld_check did not return a JSON array: %.80s\n", diagnostics);
        std::abort();
    }
    ld_free(diagnostics);
    return 0;
}
