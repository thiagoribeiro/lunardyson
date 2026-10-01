/* Plain-C consumer of the ABI; built with the sanitizers to catch leaks and
 * memory errors across many executions, tool round-trips, errors and checks. */
#include "lunardyson.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg)                                                                                            \
    do                                                                                                              \
    {                                                                                                               \
        if (!(cond))                                                                                                \
        {                                                                                                           \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);                                                 \
            return 1;                                                                                               \
        }                                                                                                           \
    } while (0)

static const char* PROGRAM = "function run(input)\n"
                             "  local total = 0\n"
                             "  for i = 1, input.n do\n"
                             "    local r = tools.math.double({ v = i })\n"
                             "    total += r.v\n"
                             "  end\n"
                             "  return { total = total }\n"
                             "end\n";

int main(void)
{
    ld_limits limits = ld_limits_default();
    limits.memory_limit_bytes = 2 * 1024 * 1024;
    ld_runtime* rt = ld_runtime_new(&limits);
    CHECK(rt, "runtime");
    CHECK(ld_tool_register(rt, "math.double", "(args: { v: number }) -> { v: number }", LD_EFFECT_PURE, LD_UNLIMITED,
                           0) == 0,
          "register");
    CHECK(ld_declare_types(rt, "type Unused = number") == 0, "declare");

    for (int iter = 0; iter < 2000; ++iter)
    {
        ld_exec* ex = NULL;
        ld_status st = ld_exec_start(rt, PROGRAM, strlen(PROGRAM), "{\"n\": 5}", NULL, &ex);
        while (st == LD_PENDING_TOOL)
        {
            ld_call call;
            CHECK(ld_exec_pending(ex, &call), "pending");
            double v = 0;
            sscanf(call.args_json, "{\"v\":%lf}", &v);
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "{\"v\": %g}", v * 2);
            st = ld_exec_resume(ex, buf, (size_t)n);
        }
        CHECK(st == LD_DONE, "done");
        size_t len = 0;
        const char* out = ld_exec_output(ex, &len);
        CHECK(strncmp(out, "{\"total\":30}", len) == 0, "output");
        ld_exec_free(ex);
    }

    /* Error paths must not leak either. */
    const char* bad[] = {"function run( end", "function run() while true do end end",
                         "function run() return tools.nope.x() end", "local function run() return {} end",
                         "function run() return { s = string.rep('x', 2^30) } end"};
    ld_error_kind want[] = {LD_ERR_SYNTAX, LD_ERR_TIMEOUT, LD_ERR_UNKNOWN_TOOL, LD_ERR_CONTRACT, LD_ERR_MEMORY};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        ld_exec* ex = NULL;
        CHECK(ld_exec_start(rt, bad[i], strlen(bad[i]), NULL, NULL, &ex) == LD_ERROR, "expected error");
        const char* msg = NULL;
        CHECK(ld_exec_error(ex, &msg) == want[i], msg);
        ld_exec_free(ex);
    }

    /* Abandon an execution mid-tool-call. */
    ld_exec* abandoned = NULL;
    CHECK(ld_exec_start(rt, PROGRAM, strlen(PROGRAM), "{\"n\": 3}", NULL, &abandoned) == LD_PENDING_TOOL, "pending");
    ld_exec_free(abandoned);

    for (int i = 0; i < 50; ++i)
    {
        char* diags = ld_check(rt, PROGRAM, strlen(PROGRAM), 1);
        CHECK(diags, "check");
        ld_free(diags);
    }

    ld_runtime_free(rt);
    printf("ok\n");
    return 0;
}
