// ld_check: static analysis with Luau.Analysis. The registered tool signatures
// and declared types are loaded as a definition file, so programs are checked
// against exactly the capabilities the runtime will expose.
#include "internal.h"

#include "Luau/BuiltinDefinitions.h"
#include "Luau/ConfigResolver.h"
#include "Luau/Error.h"
#include "Luau/Frontend.h"
#include "Luau/Linter.h"

#include "yyjson.h"

#include <map>

namespace
{

const char* const kModule = "program";

// Must mirror the globals removed from the runtime (runtime.cpp).
const char* const kHiddenGlobals[] = {
    "getfenv", "setfenv", "loadstring", "load", "require", "newproxy", "gcinfo", "collectgarbage", "os", "debug",
    "coroutine", "io", "package", "dofile", "loadfile", "buffer", "vector",
};

struct MemoryFileResolver : Luau::FileResolver
{
    std::string source;

    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override
    {
        if (name == kModule)
            return Luau::SourceCode{source, Luau::SourceCode::Module};
        return std::nullopt;
    }
};

struct Diagnostic
{
    int line;
    int column;
    std::string kind;
    std::string severity; // "error" | "lint"
    std::string message;
};

// Only exported aliases of a definition file reach the global scope, so hosts
// may write plain `type X = ...` and we export it for them.
std::string export_type_aliases(const std::string& decls)
{
    std::string out;
    size_t start = 0;
    while (start <= decls.size())
    {
        size_t end = decls.find('\n', start);
        std::string line = decls.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t indent = line.find_first_not_of(" \t");
        if (indent != std::string::npos && line.compare(indent, 5, "type ") == 0)
            line.insert(indent, "export ");
        out += line;
        if (end == std::string::npos)
            break;
        out += '\n';
        start = end + 1;
    }
    return out;
}

std::string definitions_for(const ld_runtime* rt)
{
    std::string defs = export_type_aliases(rt->type_decls);
    if (rt->tools.empty())
        return defs;

    std::map<std::string, std::vector<const ToolDef*>> by_ns;
    for (const ToolDef& t : rt->tools)
        by_ns[t.ns].push_back(&t);

    defs += "\ndeclare tools: {\n";
    for (const auto& [ns, tools] : by_ns)
    {
        defs += "    " + ns + ": {\n";
        for (const ToolDef* t : tools)
            defs += "        " + t->fn + ": " + t->signature + ",\n";
        defs += "    },\n";
    }
    defs += "}\n";
    return defs;
}

char* to_json(const std::vector<Diagnostic>& diags)
{
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* arr = yyjson_mut_arr(doc);
    for (const Diagnostic& d : diags)
    {
        yyjson_mut_val* o = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, o, "line", d.line);
        yyjson_mut_obj_add_int(doc, o, "column", d.column);
        yyjson_mut_obj_add_strcpy(doc, o, "kind", d.kind.c_str());
        yyjson_mut_obj_add_strcpy(doc, o, "severity", d.severity.c_str());
        yyjson_mut_obj_add_strcpy(doc, o, "message", d.message.c_str());
        yyjson_mut_arr_append(arr, o);
    }
    yyjson_mut_doc_set_root(doc, arr);
    char* out = yyjson_mut_write(doc, 0, nullptr);
    yyjson_mut_doc_free(doc);
    return out;
}

} // namespace

struct Checker
{
    MemoryFileResolver files;
    Luau::NullConfigResolver config;
    std::unique_ptr<Luau::Frontend> frontend;
    std::vector<Diagnostic> definition_errors;

    explicit Checker(const ld_runtime* rt)
    {
        Luau::FrontendOptions options;
        options.runLintChecks = true;
        frontend = std::make_unique<Luau::Frontend>(Luau::SolverMode::New, &files, &config, options);

        Luau::GlobalTypes& globals = frontend->globals;
        Luau::registerBuiltinGlobals(*frontend, globals);

        std::string defs = definitions_for(rt);
        if (!defs.empty())
        {
            Luau::LoadDefinitionFileResult r =
                frontend->loadDefinitionFile(globals, globals.globalScope, defs, "@lunardyson", false, false);
            if (!r.success)
            {
                for (const Luau::ParseError& e : r.parseResult.errors)
                    definition_errors.push_back({int(e.getLocation().begin.line + 1), int(e.getLocation().begin.column + 1),
                                                 "DefinitionError", "error", e.getMessage()});
                if (r.module)
                    for (const Luau::TypeError& e : r.module->errors)
                        definition_errors.push_back({int(e.location.begin.line + 1), int(e.location.begin.column + 1),
                                                     "DefinitionError", "error", Luau::toString(e)});
            }
        }

        for (const char* name : kHiddenGlobals)
        {
            Luau::AstName n = globals.globalNames.names->get(name);
            if (n.value)
                globals.globalScope->bindings.erase(Luau::Symbol(n));
        }
        Luau::freeze(globals.globalTypes);
    }

    std::vector<Diagnostic> check(std::string source, bool strict)
    {
        files.source = std::move(source);
        config.defaultConfig.mode = strict ? Luau::Mode::Strict : Luau::Mode::Nonstrict;
        frontend->markDirty(kModule);

        Luau::CheckResult result = frontend->check(kModule);
        std::vector<Diagnostic> out = definition_errors;
        for (const Luau::TypeError& e : result.errors)
        {
            Diagnostic d{int(e.location.begin.line + 1), int(e.location.begin.column + 1), "TypeError", "error", ""};
            if (const auto* syntax = Luau::get_if<Luau::SyntaxError>(&e.data))
            {
                d.kind = "SyntaxError";
                d.message = syntax->message;
            }
            else
            {
                d.message = Luau::toString(e, Luau::TypeErrorToStringOptions{&files});
            }
            out.push_back(std::move(d));
        }
        for (const auto* bucket : {&result.lintResult.errors, &result.lintResult.warnings})
            for (const Luau::LintWarning& w : *bucket)
                out.push_back({int(w.location.begin.line + 1), int(w.location.begin.column + 1),
                               Luau::LintWarning::getName(w.code), "lint", w.text});
        return out;
    }
};

void ld_checker_free(Checker* c)
{
    delete c;
}

extern "C" LD_API char* ld_check(ld_runtime* rt, const char* source, size_t source_len, int strict)
{
    if (!rt || !source)
        return nullptr;
    try
    {
        ld_runtime_seal(rt);
        if (!rt->checker)
            rt->checker = new Checker(rt);
        return to_json(rt->checker->check(std::string(source, source_len), strict != 0));
    }
    catch (const std::exception& e)
    {
        return to_json({{0, 0, "InternalError", "error", e.what()}});
    }
    catch (...)
    {
        return to_json({{0, 0, "InternalError", "error", "analysis failed"}});
    }
}
