// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What the language needs to know about characters beyond ASCII: case, identifiers, and the properties `\p{...}` names.
// The tables are generated; see tools/gen_unicode.py.
#pragma once

#include <string_view>
#include <utility>
#include <vector>

namespace script {
namespace internal {
namespace dyn {
namespace unicode {

using Ranges = std::vector<std::pair<char32_t, char32_t>>;

bool isIdStart(char32_t c);
bool isIdContinue(char32_t c);

char32_t simpleUpper(char32_t c);
char32_t simpleLower(char32_t c);
/// Simple case folding: common and simple mappings of CaseFolding.txt.
char32_t simpleFold(char32_t c);
/// The full mapping `toUpperCase` makes, which can be longer than what it maps: how many it wrote.
int fullUpper(char32_t c, char32_t out[3]);

/// Canonicalize of a pattern without the `u` flag: the upper case, unless that is several characters or leaves
/// non-ASCII for ASCII.
char32_t legacyCanonical(char32_t c);

/// Every code point that canonicalizes the way this one does, in either mode, itself included. Null when there are
/// none but itself.
const std::vector<char32_t>* caseEquivalents(char32_t c, bool foldMode);
/// Calls `each` on every set of two or more code points that canonicalize alike.
template <class Fn>
void forEachCaseGroup(bool foldMode, Fn&& each);
const std::vector<std::vector<char32_t>>& caseGroups(bool foldMode);
template <class Fn>
void forEachCaseGroup(bool foldMode, Fn&& each)
{
    for (const std::vector<char32_t>& group : caseGroups(foldMode))
        each(group);
}

/// `\p{name}` or `\p{name=value}`, into `out`; false when no such property is known. In a build without the tables only
/// the few properties that take no table are known.
bool lookupProperty(std::string_view name, std::string_view value, Ranges& out);

}  // namespace unicode
}  // namespace dyn
}  // namespace internal
}  // namespace script
