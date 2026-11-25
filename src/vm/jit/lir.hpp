// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The lowered IR of the SSA tier's new back end (switched by SSA_LIR). It is built from the SSA graph (ssa.hpp) after the
// representation plan: one LIR block per SSA block, nodes in SSA form. An instruction the back end does not lower is one
// `Generic` node, which runs the instruction's template as the old emitter does (`ssaGeneric`). Each node that may leave
// the code names the bytecode instruction the baseline code goes on at; what each live register holds there comes from
// the SSA graph, and each SSA value maps to the node that makes it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace script {
namespace internal {
namespace vm {
namespace lir {

inline constexpr std::uint32_t kNone = ~0u;

enum class Type : std::uint8_t { None, I32, F64, Tagged, Bool };

enum class Op : std::uint8_t
{
    Const,    ///< `imm` is the boxed value (`type` Tagged), or an int32 / the bits of a double
    Entry,    ///< the value a register holds at the function's entry (`imm`: the register)
    Phi,      ///< `args`: one node per predecessor of the block, in its order
    Generic,  ///< bytecode instruction `at`, by its template; defines the SSA values it writes (as `Tagged`)
    Jump,     ///< to the block's only successor
    Return,   ///< `args[0]`
};

/// Abstract heap regions a node reads or writes (bits).
enum Region : std::uint32_t
{
    kNoRegion = 0,
    kWorld = 1u << 0,  ///< anything: a call, a template, a store the IR does not model
};

struct Node
{
    Op op = Op::Const;
    Type type = Type::None;
    std::uint32_t at = kNone;      ///< the bytecode instruction it comes from
    std::uint32_t def = kNone;     ///< the SSA value it makes (kNone: none, or several for `Generic`)
    std::int64_t imm = 0;
    std::vector<std::uint32_t> args;
    std::uint32_t reads = kNoRegion, writes = kNoRegion;
    bool mayExit = false;
};

struct Block
{
    std::uint32_t ssa = 0;  ///< the SSA block it lowers
    std::vector<std::uint32_t> nodes;
    std::vector<std::uint32_t> preds, succs;
    bool loopHeader = false;
};

struct Func
{
    std::vector<Node> nodes;
    std::vector<Block> blocks;               ///< blocks[b] lowers SSA block b (0: the way in; unreached ones are empty)
    std::vector<std::uint32_t> valueNode;    ///< per SSA value: the node that makes it (kNone: none yet)
    std::uint32_t add(Node n)
    {
        nodes.push_back(std::move(n));
        return std::uint32_t(nodes.size() - 1);
    }
};

const char* opName(Op op);
std::string dump(const Func& f);

}  // namespace lir
}  // namespace vm
}  // namespace internal
}  // namespace script
