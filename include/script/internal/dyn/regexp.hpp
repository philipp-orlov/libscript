// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The regular expression engine: a pattern compiled to a small instruction set, run by a backtracking matcher whose
// choice points live on a stack of its own. Nothing here knows about objects; builtins/regexp.cpp is the language's side.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace script {
namespace internal {
namespace dyn {
namespace regex {

enum Flag : unsigned
{
    kHasIndices = 1u << 0,
    kGlobal = 1u << 1,
    kIgnoreCase = 1u << 2,
    kMultiline = 1u << 3,
    kDotAll = 1u << 4,
    kUnicode = 1u << 5,
    kUnicodeSets = 1u << 6,
    kSticky = 1u << 7,
};

/// False when these are not flags a regular expression can have.
bool parseFlags(std::u16string_view text, unsigned& out);

struct NamedGroup
{
    std::u16string name;
    std::vector<std::uint32_t> groups;  ///< more than one when alternatives share the name
};

struct Program;
/// Null with `error` set when the pattern is not one.
std::shared_ptr<const Program> compile(std::u16string_view pattern, unsigned flags, std::string& error);
/// True, with what is wrong in `error`, when a literal cannot be compiled.
bool literalError(std::u16string_view pattern, std::u16string_view flags, std::string& error);
std::uint32_t groupCount(const Program& program);  ///< with the whole match as group 0
const std::vector<NamedGroup>& namedGroups(const Program& program);

/// What a match needs and can use again. `poll`, when set, is told every so often how much the match holds, and may
/// throw to end it.
struct Matcher
{
    std::vector<std::int32_t> registers;
    std::vector<std::uint32_t> stack;
    void (*poll)(void* context, std::size_t bytes) = nullptr;
    void* pollContext = nullptr;
};

/// Looks for a match at or after `start` (at it only, when sticky). The groups' bounds are then the first registers, two
/// a group, -1 for one that took no part.
bool exec(const Program& program, std::u16string_view input, std::size_t start, Matcher& matcher);

}  // namespace regex
}  // namespace dyn
}  // namespace internal
}  // namespace script
