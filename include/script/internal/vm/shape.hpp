// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Shapes: what objects with the same properties added in the same order share. A shape names the last property added,
// its parent, and (unlike the dynamic engine's) the prototype, so one compare of an object's shape id validates the
// object and its prototype together. An object has no prototype field.
#pragma once

#include <cstdint>

#include "base/hash.hpp"
#include "script/internal/common.hpp"
#include "script/internal/vm/cell.hpp"

namespace script {
namespace internal {
namespace vm {

struct VM;
struct JSObject;
struct Shape;

/// A property name: an interned string or a symbol, compared by address.
using Key = detail::GCObject*;

namespace attr {
inline constexpr std::uint8_t kWritable = 1;
inline constexpr std::uint8_t kEnumerable = 2;
inline constexpr std::uint8_t kConfigurable = 4;
/// The slot holds a pointer to a `GetterSetter` cell.
inline constexpr std::uint8_t kAccessor = 8;
inline constexpr std::uint8_t kDefault = kWritable | kEnumerable | kConfigurable;
}  // namespace attr

inline constexpr std::uint32_t kNoShape = 0;
inline constexpr std::uint32_t kDefaultMaxShapes = 1u << 20;
/// Shape ids a context can hand out over its life: ids of shapes that died with their prototype are not used again (a cache
/// may still name one), so a program that makes short-lived prototypes in a loop spends them. Only the table's address
/// space is reserved up front; its pages are committed as the ids are used.
inline constexpr std::uint32_t kShapeIdSpace = 1u << 23;
/// A shape id is the table index in its low 23 bits and, above them, how many times that index has been used before: a cache
/// or compiled code that names the id of a dead shape never matches the shape that takes its place in the table.
inline constexpr std::uint32_t kShapeIndexBits = 23;
inline constexpr std::uint32_t kShapeIndexMask = kShapeIdSpace - 1;
/// Properties an object may have before it becomes a dictionary instead of growing its shape tree.
inline constexpr std::uint32_t kMaxFastProperties = 512;
/// Properties an object may have when it changes prototype and keeps a shape of the new tree (they are replayed there).
inline constexpr std::uint32_t kMaxReplayedProperties = 64;
/// Shapes with this many properties or fewer find one by walking the parent chain.
inline constexpr std::uint32_t kScanLimit = 8;
/// Transitions out of one shape for names made at run time (not spelled in a program's source) before the objects
/// that would add another become dictionaries.
inline constexpr std::uint32_t kMaxRuntimeTransitions = 32;

/// An open-addressed table of 32-bit entries, where 0 is empty and anything else is an index plus one into an array the
/// owner keeps. Storage only: the owner hashes and compares.
class IndexTable
{
public:
    bool empty() const noexcept { return slots_.empty(); }
    void clear() noexcept
    {
        slots_.clear();
        slots_.shrink_to_fit();
        used_ = 0;
    }
    /// Make room for `entries` without a rebuild; the owner re-inserts everything.
    void reset(std::size_t entries)
    {
        slots_.assign(base::tableSizeFor(entries * 2, 8), 0);
        used_ = 0;
    }
    std::size_t capacity() const noexcept { return slots_.size(); }
    std::size_t size() const noexcept { return used_; }
    template <class Match>
    std::uint32_t find(std::size_t hash, Match&& match) const noexcept
    {
        if (slots_.empty())
            return 0;
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t i = hash & mask;; i = (i + 1) & mask) {
            const std::uint32_t s = slots_[i];
            if (!s)
                return 0;
            if (match(s - 1))
                return s;
        }
    }
    /// Insert `index`; false when the table is too full and the owner must rebuild it larger.
    bool insert(std::size_t hash, std::uint32_t index) noexcept
    {
        if ((used_ + 1) * 2 > slots_.size())
            return false;
        const std::size_t mask = slots_.size() - 1;
        std::size_t i = hash & mask;
        while (slots_[i])
            i = (i + 1) & mask;
        slots_[i] = index + 1;
        ++used_;
        return true;
    }

private:
    Vec<std::uint32_t> slots_;
    std::size_t used_ = 0;
};

inline std::uint64_t keyBit(Key key) noexcept
{
    return std::uint64_t(1) << (base::hashPointer(key) & 63);
}

struct Transition
{
    Key key;
    std::uint8_t attrs;
    Shape* next;
};

struct Entry
{
    Key key;
    std::uint8_t attrs;
};

/// An object's own properties once it has left the shape tree: in insertion order, deleted ones left as holes so slot
/// indexes stay put, found through a hash table.
struct Dictionary
{
    Vec<Entry> entries;  ///< index = slot index; a null key is a hole
    IndexTable table;
    std::uint32_t live = 0;
    /// Cleared when it gains a key: the megamorphic table's entries that read a prototype's property through it hold it.
    struct ValidityCell* absent = nullptr;
    std::uint16_t misses = 0;  ///< reads an inline cache could not take: a dictionary read often is worth caching
    std::uint8_t changes = 0;  ///< times a cached shape had to be given up (a key moved or went)

    /// The slot index of `key`, or -1.
    std::int32_t find(Key key) const noexcept;
    /// Append; the caller has checked `key` is absent.
    std::uint32_t add(Key key, std::uint8_t attrs);
    void erase(std::uint32_t index) noexcept;
    void rebuild();
};

struct Shape
{
    enum Flags : std::uint8_t
    {
        kDictionary = 1,  ///< private to one object; inline caches never cache it
        kHasTable = 2,    ///< `table` and `lookup` are built
        kRoot = 4,
        /// Property 2 is `prototype`, a data property: `new` of a function of this shape reads it there. Shapes grown from
        /// one keep it (a property is never taken away from the middle of a shape).
        kProtoAt2 = 8,
        /// A dictionary an inline cache may hold: its own keys keep their slots for as long as it has this id (a key that
        /// goes, moves or changes its attributes gives the object a new one), and the id is never given out again.
        kCachedDict = 16,
    };

    std::uint32_t id = 0;
    std::uint32_t count = 0;  ///< properties
    JSObject* proto = nullptr;  ///< counted by the root only
    Shape* parent = nullptr;
    Shape* root = nullptr;
    Key key = nullptr;  ///< the last property added
    std::uint8_t attrs = 0;
    std::uint8_t flags = 0;
    std::uint8_t runtimeTransitions = 0;
    /// A root's: the inline slots `new` gives an object of its tree, once one has grown past those it was made with (8, 16
    /// or 24; 0 before).
    std::uint8_t slotHint = 0;
    /// The inline slots every object of the shape has: a shape says where each of its properties is. Objects with one
    /// prototype and different numbers of inline slots are in different trees.
    std::uint8_t aux = 0;
    /// A root's: the first root made for its prototype (`Rare::childRoot`, which has the hint and the ids for all), and the
    /// next one of that prototype.
    bool hintTried = false;  ///< a first root's: `newSlotsOf` has looked at the text
    Shape* base = nullptr;
    Shape* nextAux = nullptr;
    /// A first root's: the ids of the roots for 3, 8, 16 and 24 inline slots (0: none yet), where `new` finds them.
    std::uint32_t auxIds[4] = {};
    static constexpr int auxIndex(std::uint8_t aux) noexcept { return aux == 3 ? 0 : aux == 8 ? 1 : aux == 16 ? 2 : aux == 24 ? 3 : -1; }
    std::uint64_t keyBits = 0;  ///< a bit per key hash: a clear bit means absent
    Vec<Transition> transitions;
    IndexTable transitionIndex;
    Vec<Entry> table;  ///< one per property, in order, built on the first lookup of a shape past `kScanLimit`
    IndexTable lookup;

    bool isDictionary() const noexcept { return (flags & kDictionary) != 0; }
    /// The property index of `key`, or -1; `attrs` receives its attributes. Not for dictionaries (see `Dictionary`).
    std::int32_t indexOf(Key key, std::uint8_t* attrs = nullptr);
    /// The key and attributes of property `index`.
    Entry entryAt(std::uint32_t index);
    /// The shape that has this one's properties plus `key`; null when the tree may not grow any more (too many shapes
    /// or too many run-time names), and the object has to become a dictionary.
    Shape* after(VM& vm, Key key, std::uint8_t attrs);
    /// The shape with the last property's attributes changed.
    Shape* withLastAttrs(VM& vm, std::uint8_t newAttrs);
    /// The shape `after` has made before for `key` with `attrs`, or null. One exists only when this shape lacks `key`.
    Shape* findTransition(Key key, std::uint8_t attrs) const noexcept;

private:
    void buildTable();
};

}  // namespace vm
}  // namespace internal
}  // namespace script
