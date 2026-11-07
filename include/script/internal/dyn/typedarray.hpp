// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// ArrayBuffer and the typed views over it.
#pragma once

#include "script/internal/dyn/runtime.hpp"

namespace script {
namespace internal {
namespace dyn {

enum class ElementKind : std::uint8_t
{
    Int8,
    Uint8,
    Uint8Clamped,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Float16,
    Float32,
    Float64,
    BigInt64,
    BigUint64,
    Count,
};

struct DynTypedArray;

struct DynArrayBuffer : DynObject
{
    std::uint8_t* data = nullptr;
    std::size_t byteLength = 0;
    std::size_t maxByteLength = 0;
    bool resizable = false;
    bool detached = false;
    bool shared = false;
    /// The views over it, which know its length and where it is. Not counted: a view holds its buffer, not the other way.
    std::vector<DynTypedArray*> views;

    ~DynArrayBuffer() override;
    std::size_t heapSize() const override;
    void clearReferences() override;
};

struct DynTypedArray : DynObject
{
    /// Where the elements are and how many, right now: zero, once the buffer is detached or too short for this view. All a
    /// read of an element looks at.
    std::uint8_t* elements = nullptr;
    std::size_t length = 0;
    Ref<DynArrayBuffer> buffer;
    std::size_t byteOffset = 0;
    std::size_t fixedLength = 0;
    ElementKind element = ElementKind::Uint8;
    bool tracksLength = false;
    bool outOfBounds = false;

    ~DynTypedArray() override;
    void children(GcList& out) const override;
    void clearReferences() override;
    /// Work out `elements` and `length` again.
    void refresh() noexcept;
};

struct DynDataView : DynObject
{
    Ref<DynArrayBuffer> buffer;
    std::size_t byteOffset = 0;
    std::size_t fixedLength = 0;
    bool tracksLength = false;

    void children(GcList& out) const override;
    void clearReferences() override;
};

/// When the key names an element, whether there is such an element or not: CanonicalNumericIndexString. `index` is -1 for
/// one that can never be one, such as "1.5" or "-0".
bool numericKey(const GCObject* key, double& index);

Value typedArrayGet(Runtime& rt, const DynTypedArray* array, std::size_t index);
/// Converts first, whether the index turns out to be there or not, as the language has it.
void typedArraySet(Runtime& rt, DynTypedArray* array, double index, const Value& v);
inline bool typedArrayHas(const DynTypedArray* array, double index) noexcept { return index >= 0 && index < double(array->length); }

/// What a program gets for `$262.detachArrayBuffer` and `transfer`.
void detachBuffer(Runtime& rt, DynArrayBuffer* buffer);

}  // namespace dyn
}  // namespace internal
}  // namespace script
