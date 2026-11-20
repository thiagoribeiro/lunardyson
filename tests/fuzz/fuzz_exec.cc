// libFuzzer target: run arbitrary Luau through the step API and check the runtime's
// guarantees on every step, not only "does not crash".
//
// Input layout: <program source> [ "\n--8<--\n" <tool reply> ]...
// Each pending tool call consumes the next reply. A reply starting with '!' is
// delivered as ld_exec_resume_error(rest); anything else goes to ld_exec_resume as
// raw bytes (so malformed JSON is exercised too). With no replies left the tool
// returns {"ok":true}.
//
// Invariants (abort == finding):
//   - status is always DONE, PENDING_TOOL or ERROR
//   - a pending call names a registered tool, carries its declared effect, and
//     call_id increases by exactly 1 from 1
//   - budgets hold: per-tool max_calls, money budget 0 (pay.charge never reaches the
//     host), external_write budget 2
//   - an ERROR has a known error kind and a message; DONE has a JSON output
//   - VM time stays near the CPU budget and peak memory under the memory budget
//   - the runtime is reusable: every execution starts on the same runtime

#include "lunardyson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr uint32_t kCpuMs = 50;
constexpr size_t kMemBytes = 4u << 20;
// Deadlines are enforced at VM safepoints; one long builtin may overrun (documented).
// Anything beyond this is a missed deadline, not jitter.
constexpr uint64_t kVmTimeCeilingUs = 2'000'000;
constexpr size_t kMemSlack = 256u << 10;
constexpr int kMaxSteps = 256;

struct Tool
{
    const char* name;
    const char* signature;
    ld_effect effect;
    uint32_t max_calls;
    size_t max_response_bytes;
};

// Names match tests/conformance/cases.json so conformance seeds reach real tools.
const Tool kTools[] = {
    {"crm.get", "(args: { id: string }) -> any", LD_EFFECT_READ, 8, 4096},
    {"crm.update", "(args: any) -> any", LD_EFFECT_WRITE, 4, 4096},
    {"data.get", "(args: any) -> any", LD_EFFECT_READ, 8, 4096},
    {"data.big", "(args: any) -> any", LD_EFFECT_READ, 3, 1000},
    {"pay.charge", "(args: any) -> any", LD_EFFECT_MONEY, 5, 4096},
    {"mail.send", "(args: any) -> any", LD_EFFECT_EXTERNAL_WRITE, 5, 4096},
};

[[noreturn]] void finding(const char* what, const std::string& detail = "")
{
    std::fprintf(stderr, "INVARIANT VIOLATED: %s %s\n", what, detail.c_str());
    std::abort();
}

ld_runtime* runtime()
{
    static ld_runtime* rt = [] {
        ld_limits limits = ld_limits_default();
        limits.cpu_time_ms = kCpuMs;
        limits.memory_limit_bytes = kMemBytes;
        limits.max_effects[LD_EFFECT_MONEY] = 0;
        limits.max_effects[LD_EFFECT_EXTERNAL_WRITE] = 2;
        ld_runtime* r = ld_runtime_new(&limits);
        if (!r)
            finding("ld_runtime_new failed");
        for (const Tool& t : kTools)
            if (ld_tool_register(r, t.name, t.signature, t.effect, t.max_calls, t.max_response_bytes) != 0)
                finding("ld_tool_register failed", t.name);
        return r;
    }();
    return rt;
}

const Tool* find_tool(const char* name)
{
    for (const Tool& t : kTools)
        if (name && std::strcmp(t.name, name) == 0)
            return &t;
    return nullptr;
}

std::vector<std::string_view> split(std::string_view data)
{
    static constexpr std::string_view kSep = "\n--8<--\n";
    std::vector<std::string_view> parts;
    size_t pos = 0;
    while (true)
    {
        size_t next = data.find(kSep, pos);
        if (next == std::string_view::npos)
        {
            parts.push_back(data.substr(pos));
            return parts;
        }
        parts.push_back(data.substr(pos, next - pos));
        pos = next + kSep.size();
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::vector<std::string_view> parts = split(std::string_view(reinterpret_cast<const char*>(data), size));
    const std::string source(parts[0]);
    size_t reply = 1;

    ld_exec* ex = nullptr;
    ld_status st = ld_exec_start(runtime(), source.data(), source.size(), "{\"n\":3,\"id\":\"c1\"}", "{}", &ex);
    if (!ex)
        finding("ld_exec_start returned no execution (previous one leaked?)");

    std::map<std::string, uint32_t> calls;
    uint32_t external_writes = 0;
    uint64_t expected_id = 1;

    for (int steps = 0; st == LD_PENDING_TOOL; ++steps)
    {
        if (steps > kMaxSteps)
            finding("more pending calls than every tool budget allows");

        ld_call call{};
        if (ld_exec_pending(ex, &call) != 1)
            finding("status PENDING_TOOL but ld_exec_pending found no call");
        const Tool* tool = find_tool(call.tool);
        if (!tool)
            finding("pending call to an unregistered tool", call.tool ? call.tool : "(null)");
        if (call.effect != tool->effect)
            finding("pending call carries the wrong effect", tool->name);
        if (call.call_id != expected_id++)
            finding("call_id is not sequential");
        if (++calls[tool->name] > tool->max_calls)
            finding("tool called past its max_calls", tool->name);
        if (tool->effect == LD_EFFECT_MONEY)
            finding("money call reached the host with a money budget of 0");
        if (tool->effect == LD_EFFECT_EXTERNAL_WRITE && ++external_writes > 2)
            finding("external_write budget exceeded");

        if (reply < parts.size())
        {
            std::string_view r = parts[reply++];
            if (!r.empty() && r[0] == '!')
                st = ld_exec_resume_error(ex, std::string(r.substr(1)).c_str());
            else
                st = ld_exec_resume(ex, r.data(), r.size());
        }
        else
        {
            static constexpr char kOk[] = "{\"ok\":true}";
            st = ld_exec_resume(ex, kOk, sizeof(kOk) - 1);
        }
    }

    if (st == LD_DONE)
    {
        size_t len = 0;
        const char* out = ld_exec_output(ex, &len);
        if (!out || len == 0 || (out[0] != '{' && out[0] != '['))
            finding("DONE without a JSON object/array output");
    }
    else if (st == LD_ERROR)
    {
        const char* msg = nullptr;
        ld_error_kind kind = ld_exec_error(ex, &msg);
        if (kind <= LD_ERR_NONE || kind > LD_ERR_INVALID_ARG)
            finding("ERROR with an unknown error kind", std::to_string(kind));
        if (!msg)
            finding("ERROR without a message");
    }
    else
    {
        finding("unknown status", std::to_string(st));
    }

    ld_stats stats = ld_exec_stats(ex);
    if (stats.vm_time_us > kVmTimeCeilingUs)
        finding("VM time far past the CPU budget", std::to_string(stats.vm_time_us) + "us");
    if (stats.peak_memory_bytes > kMemBytes + kMemSlack)
        finding("peak memory above the memory budget", std::to_string(stats.peak_memory_bytes));

    ld_exec_free(ex);
    return 0;
}
