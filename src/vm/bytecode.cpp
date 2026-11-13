// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/vm/bytecode.hpp"

#include <cstdio>

namespace script {
namespace internal {
namespace vm {

namespace {

const char* const kNames[] = {
#define OP(name, kinds) #name,
#include "script/internal/vm/opcodes.def"
#undef OP
};

const char* const kKinds[] = {
#define OP(name, kinds) kinds,
#include "script/internal/vm/opcodes.def"
#undef OP
};

}  // namespace

const char* opName(Op op) noexcept
{
    return std::size_t(op) < std::size_t(Op::Count) ? kNames[std::size_t(op)] : "?";
}

const char* opKinds(Op op) noexcept
{
    return std::size_t(op) < std::size_t(Op::Count) ? kKinds[std::size_t(op)] : "---";
}

std::string dump(const CodeBlock& block)
{
    std::string out;
    char line[160];
    for (std::size_t pc = 0; pc < block.code.size(); ++pc) {
        const Ins& ins = block.code[pc];
        const char* kinds = opKinds(ins.opcode());
        int n = std::snprintf(line, sizeof line, "%4zu  %-16s", pc, opName(ins.opcode()));
        const std::uint16_t operands[3] = {ins.a, ins.b, ins.c};
        for (int i = 0; i < 3 && n < int(sizeof line) - 24; ++i) {
            switch (kinds[i]) {
                case 'R': n += std::snprintf(line + n, sizeof line - n, " r%u", operands[i]); break;
                case 'K': n += std::snprintf(line + n, sizeof line - n, " k%u", operands[i]); break;
                case 'A': n += std::snprintf(line + n, sizeof line - n, " @%u", operands[i]); break;
                case 'N': n += std::snprintf(line + n, sizeof line - n, " #%u", operands[i]); break;
                case 'I': n += std::snprintf(line + n, sizeof line - n, " %d", int(std::int16_t(operands[i]))); break;
                case 'J':
                    n += std::snprintf(line + n, sizeof line - n, " ->%lld", static_cast<long long>(pc) + 1 + ins.jump());
                    i = 3;
                    break;
                default: break;
            }
        }
        out += line;
        out += '\n';
    }
    for (std::size_t i = 0; i < block.children.size(); ++i) {
        out += "child " + std::to_string(i) + ":\n";
        out += dump(*block.children[i]);
    }
    return out;
}

}  // namespace vm
}  // namespace internal
}  // namespace script
