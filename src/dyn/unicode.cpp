// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "script/internal/dyn/unicode.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>

#include "script/config.hpp"

namespace script {
namespace internal {
namespace dyn {
namespace unicode {

namespace {

struct CaseRun
{
    std::uint32_t start;
    std::uint16_t count;
    std::uint8_t stride;
    std::int32_t delta;
};

struct SpecialCase
{
    std::uint32_t code;
    std::uint32_t mapped[3];
};

struct PropertyEntry
{
    const char* names;  ///< "short|long|other"
    std::uint32_t offset;
    std::uint32_t size;
};

struct GcGroup
{
    const char* names;
    std::uint32_t mask;  ///< over the rows of kGcTable
};

#include "unicode_tables.inc"

template <std::size_t N>
char32_t mapRun(const CaseRun (&runs)[N], char32_t c)
{
    const CaseRun* end = runs + N;
    const CaseRun* run = std::upper_bound(runs, end, std::uint32_t(c), [](std::uint32_t v, const CaseRun& r) { return v < r.start; });
    if (run == runs)
        return c;
    --run;
    const std::uint32_t distance = std::uint32_t(c) - run->start;
    if (distance / run->stride >= run->count || distance % run->stride != 0)
        return c;
    return char32_t(std::int64_t(c) + run->delta);
}

Ranges decode(const unsigned char* data, std::size_t size)
{
    Ranges out;
    const unsigned char* end = data + size;
    std::uint32_t at = 0;
    const auto next = [&]() {
        std::uint32_t value = 0;
        int shift = 0;
        for (;;) {
            const unsigned char b = *data++;
            value |= std::uint32_t(b & 0x7f) << shift;
            if (!(b & 0x80))
                return value;
            shift += 7;
        }
    };
    while (data < end) {
        const std::uint32_t lo = at + next();
        const std::uint32_t hi = lo + next();
        out.emplace_back(char32_t(lo), char32_t(hi));
        at = hi + 1;
    }
    return out;
}

bool inRanges(const Ranges& ranges, char32_t c)
{
    auto it = std::upper_bound(ranges.begin(), ranges.end(), c, [](char32_t v, const std::pair<char32_t, char32_t>& r) { return v < r.first; });
    return it != ranges.begin() && c <= std::prev(it)->second;
}

struct CaseTables
{
    std::vector<std::vector<char32_t>> groups;
    std::vector<std::pair<char32_t, std::uint32_t>> groupOf;  ///< sorted
};

template <class Canon>
CaseTables buildCase(Canon&& canon)
{
    // Every character some table mentions, and what those map to, is all that can have company.
    std::map<char32_t, std::vector<char32_t>> byCanon;
    const auto note = [&](char32_t c) {
        std::vector<char32_t>& members = byCanon[canon(c)];
        if (std::find(members.begin(), members.end(), c) == members.end())
            members.push_back(c);
    };
    const auto walk = [&](const auto& runs) {
        for (const CaseRun& run : runs) {
            for (std::uint32_t i = 0; i < run.count; ++i) {
                const char32_t c = run.start + i * run.stride;
                note(c);
                note(char32_t(std::int64_t(c) + run.delta));
            }
        }
    };
    walk(kUpperRuns);
    walk(kLowerRuns);
    walk(kFoldRuns);
    CaseTables out;
    for (auto& [key, members] : byCanon) {
        if (members.size() < 2)
            continue;
        std::sort(members.begin(), members.end());
        const std::uint32_t index = std::uint32_t(out.groups.size());
        for (char32_t member : members)
            out.groupOf.emplace_back(member, index);
        out.groups.push_back(std::move(members));
    }
    std::sort(out.groupOf.begin(), out.groupOf.end());
    return out;
}

const CaseTables& caseTablesFor(bool foldMode)
{
    if (foldMode) {
        static const CaseTables folded = buildCase([](char32_t c) { return simpleFold(c); });
        return folded;
    }
    static const CaseTables legacy = buildCase([](char32_t c) { return c > 0xffff ? c : legacyCanonical(c); });
    return legacy;
}

#if SCRIPT_UNICODE_PROPERTIES
bool nameMatches(const char* names, std::string_view wanted)
{
    while (*names) {
        const char* bar = std::strchr(names, '|');
        const std::size_t size = bar ? std::size_t(bar - names) : std::strlen(names);
        if (std::string_view(names, size) == wanted)
            return true;
        if (!bar)
            return false;
        names = bar + 1;
    }
    return false;
}

template <std::size_t N>
const PropertyEntry* findEntry(const PropertyEntry (&table)[N], std::string_view wanted)
{
    for (const PropertyEntry& row : table) {
        if (nameMatches(row.names, wanted))
            return &row;
    }
    return nullptr;
}

Ranges entryRanges(const PropertyEntry& row) { return decode(kPropertyData + row.offset, row.size); }

bool generalCategory(std::string_view value, Ranges& out)
{
    if (const PropertyEntry* row = findEntry(kGcTable, value)) {
        out = entryRanges(*row);
        return true;
    }
    for (const GcGroup& group : kGcGroups) {
        if (!nameMatches(group.names, value))
            continue;
        for (std::size_t i = 0; i < sizeof(kGcTable) / sizeof(kGcTable[0]); ++i) {
            if (group.mask >> i & 1) {
                Ranges part = entryRanges(kGcTable[i]);
                out.insert(out.end(), part.begin(), part.end());
            }
        }
        std::sort(out.begin(), out.end());
        return true;
    }
    return false;
}
#endif

}  // namespace

bool isIdStart(char32_t c)
{
    static const Ranges ranges = decode(kIdStartData, sizeof(kIdStartData));
    return inRanges(ranges, c);
}

bool isIdContinue(char32_t c)
{
    static const Ranges ranges = decode(kIdContinueData, sizeof(kIdContinueData));
    return inRanges(ranges, c);
}

char32_t simpleUpper(char32_t c) { return mapRun(kUpperRuns, c); }
char32_t simpleLower(char32_t c) { return mapRun(kLowerRuns, c); }
char32_t simpleFold(char32_t c) { return mapRun(kFoldRuns, c); }

int fullUpper(char32_t c, char32_t out[3])
{
    const SpecialCase* end = kSpecialUpper + sizeof(kSpecialUpper) / sizeof(kSpecialUpper[0]);
    const SpecialCase* row = std::lower_bound(kSpecialUpper, end, std::uint32_t(c), [](const SpecialCase& r, std::uint32_t v) { return r.code < v; });
    if (row != end && row->code == c) {
        int n = 0;
        while (n < 3 && row->mapped[n])
            out[n] = row->mapped[n], ++n;
        return n;
    }
    out[0] = simpleUpper(c);
    return 1;
}

char32_t legacyCanonical(char32_t c)
{
    char32_t mapped[3];
    if (fullUpper(c, mapped) != 1)
        return c;
    if (c >= 128 && mapped[0] < 128)
        return c;
    if (mapped[0] > 0xffff)
        return c;
    return mapped[0];
}

const std::vector<char32_t>* caseEquivalents(char32_t c, bool foldMode)
{
    const CaseTables& tables = caseTablesFor(foldMode);
    auto it = std::lower_bound(tables.groupOf.begin(), tables.groupOf.end(), std::make_pair(c, std::uint32_t(0)));
    return it == tables.groupOf.end() || it->first != c ? nullptr : &tables.groups[it->second];
}

const std::vector<std::vector<char32_t>>& caseGroups(bool foldMode) { return caseTablesFor(foldMode).groups; }

bool lookupProperty(std::string_view name, std::string_view value, Ranges& out)
{
    if (value.empty()) {
        if (name == "Any") {
            out.emplace_back(0, 0x10ffff);
            return true;
        }
        if (name == "ASCII") {
            out.emplace_back(0, 0x7f);
            return true;
        }
    }
#if SCRIPT_UNICODE_PROPERTIES
    if (!value.empty()) {
        if (name == "General_Category" || name == "gc")
            return generalCategory(value, out);
        const PropertyEntry* row = nullptr;
        if (name == "Script" || name == "sc")
            row = findEntry(kScriptTable, value);
        else if (name == "Script_Extensions" || name == "scx")
            row = findEntry(kScriptExtTable, value);
        if (!row)
            return false;
        out = entryRanges(*row);
        return true;
    }
    if (name == "Assigned") {
        const PropertyEntry* unassigned = findEntry(kGcTable, "Cn");
        char32_t at = 0;
        for (const auto& [lo, hi] : entryRanges(*unassigned)) {
            if (lo > at)
                out.emplace_back(at, lo - 1);
            at = hi + 1;
        }
        if (at <= 0x10ffff)
            out.emplace_back(at, 0x10ffff);
        return true;
    }
    if (const PropertyEntry* row = findEntry(kBinaryTable, name)) {
        out = entryRanges(*row);
        return true;
    }
    return generalCategory(name, out);
#else
    return false;
#endif
}

}  // namespace unicode
}  // namespace dyn
}  // namespace internal
}  // namespace script
