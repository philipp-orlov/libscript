// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "lir.hpp"

#include <cinttypes>
#include <cstdio>

namespace script {
namespace internal {
namespace vm {
namespace lir {

const char* opName(Op op)
{
    switch (op) {
    case Op::Const: return "Const";
    case Op::Entry: return "Entry";
    case Op::Phi: return "Phi";
    case Op::Generic: return "Generic";
    case Op::CheckBounds: return "CheckBounds";
    case Op::CheckHole: return "CheckHole";
    case Op::LoadElem: return "LoadElem";
    case Op::StoreElem: return "StoreElem";
    case Op::Jump: return "Jump";
    case Op::Return: return "Return";
    }
    return "?";
}

namespace {

const char* typeName(Type t)
{
    switch (t) {
    case Type::None: return "";
    case Type::I32: return "i32";
    case Type::F64: return "f64";
    case Type::Tagged: return "val";
    case Type::Bool: return "bool";
    }
    return "?";
}

}  // namespace

std::string dump(const Func& f)
{
    std::string out;
    char buf[160];
    for (std::uint32_t b = 0; b < f.blocks.size(); ++b) {
        const Block& bl = f.blocks[b];
        if (bl.nodes.empty() && bl.preds.empty() && b != 0)
            continue;
        std::snprintf(buf, sizeof buf, "L%u%s preds", b, bl.loopHeader ? " loop" : "");
        out += buf;
        for (std::uint32_t p : bl.preds)
            out += " L" + std::to_string(p);
        out += " succs";
        for (std::uint32_t s : bl.succs)
            out += " L" + std::to_string(s);
        out += "\n";
        for (std::uint32_t id : bl.nodes) {
            const Node& n = f.nodes[id];
            std::snprintf(buf, sizeof buf, "  n%u %s %s", id, typeName(n.type), opName(n.op));
            out += buf;
            if (n.op == Op::Const || n.op == Op::Entry || n.imm) {
                std::snprintf(buf, sizeof buf, " %#" PRIx64, static_cast<std::uint64_t>(n.imm));
                out += buf;
            }
            for (std::uint32_t a : n.args)
                out += a == kNone ? " -" : " n" + std::to_string(a);
            if (n.at != kNone)
                out += " @" + std::to_string(n.at);
            if (n.def != kNone)
                out += " =v" + std::to_string(n.def);
            if (n.mayExit)
                out += " exits";
            if (n.writes & kWorld)
                out += " world";
            if (n.gone)
                out += " gone";
            out += "\n";
        }
    }
    return out;
}

}  // namespace lir
}  // namespace vm
}  // namespace internal
}  // namespace script
