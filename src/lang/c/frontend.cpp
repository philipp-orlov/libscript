// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The C family's compile and run hooks. C and C++ share the lexer, the
// parser (with a dialect flag), the `libc` natives and this file; each adds
// its registration in c.cpp and cpp/cpp.cpp. Host classes reach the parser
// as `HostClassSpec`s, spelled from the binding layer's C# signatures.
#include "script/internal/c/frontend.hpp"

#include <cctype>

#include "script/internal/c/libc.hpp"
#include "script/internal/c/parser.hpp"
#include "script/internal/typed/checker.hpp"
#include "script/internal/typed/emitter.hpp"
#include "script/internal/typed/runtime.hpp"
#include "script/internal/typed/vm.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

/// The C type a C#-spelled host signature stands for; null when C has no
/// spelling for it (generics, delegates, nullable types).
bool cTypeOf(const std::string& spelled, c::CType& out)
{
    using Kind = c::CType::Kind;
    if (spelled.size() > 2 && spelled.compare(spelled.size() - 2, 2, "[]") == 0) {
        c::CType element;
        if (!cTypeOf(spelled.substr(0, spelled.size() - 2), element))
            return false;
        out = element.kind == Kind::Char ? c::CType::of(Kind::Str) : c::CType::arrayOf(element);
        return true;
    }
    struct Named
    {
        const char* name;
        Kind kind;
    };
    static constexpr Named named[] = {
        {"void", Kind::Void},     {"bool", Kind::Bool},     {"sbyte", Kind::Char},
        {"byte", Kind::UChar},    {"short", Kind::Short},   {"ushort", Kind::UShort},
        {"int", Kind::Int},       {"uint", Kind::UInt},     {"long", Kind::Long},
        {"ulong", Kind::ULong},   {"char", Kind::Char},     {"float", Kind::Float},
        {"double", Kind::Double}, {"string", Kind::Str},
    };
    for (const Named& n : named) {
        if (spelled == n.name) {
            out = c::CType::of(n.kind);
            return true;
        }
    }
    // `object` (a std::map, std::function or unknown C++ type) and anything
    // generic or nullable has no C spelling.
    if (spelled == "object" || spelled.empty())
        return false;
    for (char ch : spelled) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.'))
            return false;
    }
    out = c::CType::structOf(fromUtf8(spelled));
    return true;
}

std::vector<c::HostClassSpec> hostClassSpecs(const ContextImpl& ctx)
{
    std::vector<c::HostClassSpec> specs;
    for (const auto& info : ctx.hostClasses) {
        c::HostClassSpec spec;
        spec.name = fromUtf8(info->name);
        for (const HostClassInfo::Member& member : info->members) {
            c::HostClassSpec::Member m;
            m.name = fromUtf8(member.spec.name);
            m.isStatic = member.spec.isStatic;
            m.isMethod = member.spec.kind == HostMember::Method || member.spec.kind == HostMember::Constructor;
            m.isConstructor = member.spec.kind == HostMember::Constructor;
            bool ok = cTypeOf(member.spec.csharpReturn, m.result);
            for (const std::string& p : member.spec.csharpParams) {
                c::CType t;
                ok = ok && cTypeOf(p, t);
                m.params.push_back(t);
            }
            if (!ok)
                continue;  // a member C cannot spell is simply not there
            if (m.isConstructor)
                spec.hasConstructor = true;
            spec.members.push_back(std::move(m));
        }
        specs.push_back(std::move(spec));
    }
    return specs;
}

}  // namespace

std::shared_ptr<CompiledUnit> compileCFamily(ContextImpl& ctx, std::string_view source,
                                             std::string_view name,
                                             std::vector<Diagnostic>& diagnostics, Language language)
{
    Runtime& rt = ctx.typed();
    rt.syncHostClasses();
    rt.beforeCompile();
    c::installLibc(rt);
    Arena arena;
    c::Parser parser(source, std::string(name), arena, hostClassSpecs(ctx),
                     language == Language::Cpp ? c::Dialect::Cpp : c::Dialect::C);
    CompilationUnit* unit = parser.parseUnit();

    auto assembly = std::make_shared<AssemblyImpl>();
    assembly->language = language;
    // Every function and global lands in one synthesized class, which no other unit can see.
    assembly->declaresNames = false;
    for (const TypeDecl* decl : unit->types)
        assembly->declaresNames = assembly->declaresNames || decl->name != ascii(c::kGlobalsClass);
    assembly->name = std::string(name);
    assembly->rt = &rt;

    Checker checker(rt, arena, ctx.options.compile, *assembly);
    if (!checker.check(unit, diagnostics)) {
        // The checker speaks C#; its numbering means nothing to a C or C++ program.
        for (Diagnostic& d : diagnostics) {
            d.language = language;
            d.code = 0;
        }
        return nullptr;
    }

    Emitter emitter(rt, *assembly, ctx.options.compile);
    emitter.emitUnit(unit);

    rt.assemblies.push_back(assembly);
    if (rt.jit &&
        (ctx.options.compile.native || ctx.options.jit.mode == JitOptions::Mode::Eager))
        rt.jit->compileAll(rt, *assembly);
    return assembly;
}

Value runCFamily(ContextImpl& ctx, CompiledUnit& unit, const std::vector<Value>&)
{
    Runtime& rt = ctx.typed();
    auto& image = static_cast<AssemblyImpl&>(unit);
    rt.ensureInitialized();
    if (!image.entry)
        throw Error(ErrorCode::InvalidOperation, "the program has no main()");
    return invoke(rt, image.entry, nullptr, 0);
}

}  // namespace typed
}  // namespace internal
}  // namespace script
