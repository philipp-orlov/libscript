// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Records, and the members a record declaration stands for.
//
// `record Point(int X, int Y);` is shorthand. Rather than build the members as
// trees by hand, the shorthand is expanded back into the C# it means and that
// is parsed -- the parser is right there, and a synthesized member then goes
// through exactly the same checking as a written one. The cost is one small
// parse per record; the benefit is that there is no second implementation of
// what a constructor or an equality method looks like.
#include "script/internal/typed/checker.hpp"

#include "script/internal/typed/parser.hpp"

namespace script {
namespace internal {
namespace typed {

namespace {

/// The type as it was written, so the synthesized source says the same thing.
std::string typeText(const TypeRef* ref)
{
    if (!ref)
        return "void";
    std::string out;
    if (ref->isTuple) {
        out = "(";
        for (std::size_t i = 0; i < ref->typeArgs.size(); ++i) {
            if (i)
                out += ", ";
            out += typeText(ref->typeArgs[i]);
        }
        out += ")";
    } else if (ref->predefined != Tok::Eof) {
        out = tokenName(ref->predefined);
    } else {
        for (std::size_t i = 0; i < ref->path.size(); ++i) {
            if (i)
                out += ".";
            out += toUtf8(ref->path[i]);
        }
        if (!ref->typeArgs.empty()) {
            out += "<";
            for (std::size_t i = 0; i < ref->typeArgs.size(); ++i) {
                if (i)
                    out += ", ";
                out += typeText(ref->typeArgs[i]);
            }
            out += ">";
        }
    }
    for (std::uint32_t i = 0; i < ref->arrayRank; ++i)
        out += "[]";
    return out;
}

}  // namespace

void Checker::synthesizeRecord(TypeDecl* decl)
{
    if (!decl->isRecord && !decl->isPrimaryConstructed)
        return;

    const std::string name = toUtf8(decl->name);
    // The members a record gets are the ones it did not write for itself.
    const auto declares = [decl](const char* member) {
        const String wanted = ascii(member);
        for (const MethodDecl* method : decl->methods) {
            if (method->name == wanted)
                return true;
        }
        return false;
    };
    const auto declaresProperty = [decl](const String& wanted) {
        for (const PropertyDecl& prop : decl->props) {
            if (prop.name == wanted)
                return true;
        }
        for (const FieldDecl& field : decl->fields) {
            if (field.name == wanted)
                return true;
        }
        return false;
    };

    std::string source = "class " + name + " {\n";

    // One init-only property per primary constructor parameter, and the
    // constructor that fills them.
    std::vector<std::string> names;
    std::vector<std::string> types;
    for (const ParamDecl& param : decl->params) {
        names.push_back(toUtf8(param.name));
        types.push_back(typeText(param.typeRef));
    }
    if (decl->isRecord) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (declaresProperty(decl->params[i].name))
                continue;
            source += "  public " + types[i] + " " + names[i] + " { get; init; }\n";
        }
    } else {
        // A non-record primary constructor keeps its parameters private.
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (declaresProperty(decl->params[i].name))
                continue;
            source += "  private readonly " + types[i] + " " + names[i] + ";\n";
        }
    }

    if (!decl->params.empty() && !declares(".ctor")) {
        source += "  public " + name + "(";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i)
                source += ", ";
            source += types[i] + " " + names[i] + "__";
        }
        source += ") {\n";
        for (std::size_t i = 0; i < names.size(); ++i)
            source += "    this." + names[i] + " = " + names[i] + "__;\n";
        source += "  }\n";
    }

    if (decl->isRecord) {
        // What a record compares and prints is its state: the positional
        // parameters and any field or settable property it declared. A
        // computed property is not state and is left out, the way C# does.
        std::vector<std::string> members = names;
        std::vector<std::string> memberTypes = types;
        std::vector<bool> settable(names.size(), true);
        for (const PropertyDecl& prop : decl->props) {
            if (!prop.hasGetter || !prop.hasSetter || !prop.autoProperty)
                continue;
            bool already = false;
            for (const std::string& seen : members)
                already = already || seen == toUtf8(prop.name);
            if (!already) {
                members.push_back(toUtf8(prop.name));
                memberTypes.push_back(typeText(prop.typeRef));
                settable.push_back(true);
            }
        }
        for (const FieldDecl& field : decl->fields) {
            if (field.modifiers & (ModStatic | ModConst))
                continue;
            bool already = false;
            for (const std::string& seen : members)
                already = already || seen == toUtf8(field.name);
            if (!already) {
                members.push_back(toUtf8(field.name));
                memberTypes.push_back(typeText(field.typeRef));
                settable.push_back((field.modifiers & ModReadonly) == 0);
            }
        }

        if (!names.empty() && !declares("Deconstruct")) {
            source += "  public void Deconstruct(";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i)
                    source += ", ";
                source += "out " + types[i] + " " + names[i] + "__";
            }
            source += ") {\n";
            for (std::size_t i = 0; i < names.size(); ++i)
                source += "    " + names[i] + "__ = this." + names[i] + ";\n";
            source += "  }\n";
        }

        if (!declares("ToString")) {
            source += "  public override string ToString() {\n";
            source += "    return \"" + name + " { ";
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (i)
                    source += ", ";
                source += members[i] + " = \" + " + members[i] + " + \"";
            }
            source += members.empty() ? "}\";\n" : " }\";\n";
            source += "  }\n";
        }

        if (!declares("Equals")) {
            source += "  public override bool Equals(object other__) {\n";
            source += "    " + name + " o__ = other__ as " + name + ";\n";
            source += "    if (o__ == null) return false;\n";
            for (const std::string& member : members)
                source += "    if (!object.Equals(this." + member + ", o__." + member +
                          ")) return false;\n";
            source += "    return true;\n  }\n";
        }

        if (!declares("GetHashCode")) {
            source += "  public override int GetHashCode() {\n    int h__ = 17;\n";
            for (const std::string& member : members)
                source += "    h__ = h__ * 31 + (" + member +
                          " == null ? 0 : " + member + ".GetHashCode());\n";
            source += "    return h__;\n  }\n";
        }

        // `with` copies through this, then overwrites what was named.
        source += "  public " + name + " " + kRecordClone + "() {\n";
        source += "    " + name + " c__ = new " + name + "(";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i)
                source += ", ";
            source += "this." + names[i];
        }
        source += ");\n";
        for (std::size_t i = names.size(); i < members.size(); ++i) {
            if (settable[i])
                source += "    c__." + members[i] + " = this." + members[i] + ";\n";
        }
        source += "    return c__;\n  }\n";
    }

    source += "}\n";

    // Parse the expansion and take its members. A failure here is a bug in
    // this file, not in the program being compiled, so it is reported against
    // the declaration rather than swallowed.
    CompilationUnit* expanded = nullptr;
    try {
        Parser parser(source, "<record>", arena_);
        expanded = parser.parseUnit();
    } catch (const Error& error) {
        this->error(decl->line, 1, 8858,
                    "could not expand the record '" + name + "': " + error.what());
        return;
    }
    if (!expanded || expanded->types.empty())
        return;

    TypeDecl* from = expanded->types.front();
    for (MethodDecl* method : from->methods) {
        method->owner = decl;
        method->line = decl->line;
        decl->methods.push_back(method);
    }
    for (PropertyDecl& prop : from->props) {
        prop.line = decl->line;
        decl->props.push_back(prop);
    }
    for (FieldDecl& field : from->fields) {
        field.line = decl->line;
        decl->fields.push_back(field);
    }
    // The parameters were only ever shorthand for the members above.
    decl->params.clear();
}

}  // namespace typed
}  // namespace internal
}  // namespace script
