// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Where syntax tree nodes live.
#include "script/internal/dyn/ast.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace script {
namespace internal {
namespace dyn {

namespace {

/// A block of nodes, carved in order. Each node is preceded by a pointer to its block, which it gives back to when it
/// goes; the block is freed when the last of its nodes has, and blocks are plain heap allocations, which reuse the
/// memory of those before them.
struct NodeBlock
{
    static constexpr std::size_t kBytes = std::size_t(64) << 10;
    /// While the block is carved from, `kCarving` less the nodes deleted; after, the nodes handed out and not yet deleted.
    /// The carver counts what it hands out and settles once, when it moves on. Atomic since a tree may be dropped on
    /// another thread than the one that made it.
    static constexpr std::uint32_t kCarving = std::uint32_t(1) << 30;
    std::atomic<std::uint32_t> live{kCarving};
};

constexpr std::size_t kAlign = alignof(std::max_align_t) > alignof(Node) ? alignof(std::max_align_t) : alignof(Node);
constexpr std::size_t kFirst = (sizeof(NodeBlock) + kAlign - 1) / kAlign * kAlign;
/// A node's slot: the pointer to its block, then the node.
constexpr std::size_t kHeader = (sizeof(NodeBlock*) + alignof(Node) - 1) / alignof(Node) * alignof(Node);
constexpr std::size_t kSlot = (kHeader + sizeof(Node) + alignof(Node) - 1) / alignof(Node) * alignof(Node);
static_assert(kFirst + kSlot <= NodeBlock::kBytes, "a block holds at least one node");

void release(NodeBlock* block, std::uint32_t count = 1) noexcept
{
    if (block->live.fetch_sub(count, std::memory_order_acq_rel) == count) {
        block->~NodeBlock();
        std::free(block);
    }
}

/// The block this thread carves from, and where in it.
struct Carver
{
    NodeBlock* block = nullptr;
    std::size_t used = 0;
    std::uint32_t carved = 0;
    /// Moves on from the block: it now counts only the nodes it handed out.
    void retire() noexcept { release(block, NodeBlock::kCarving - carved); }
    ~Carver()
    {
        if (block)
            retire();
    }
};

thread_local Carver carver;

}  // namespace

const NodeExtra& Node::noExtra() noexcept
{
    static const NodeExtra none;
    return none;
}

void* Node::operator new(std::size_t size)
{
#if defined(__SANITIZE_ADDRESS__)
    // One allocation each, for the sanitizer to watch.
    return ::operator new(size);
#endif
    (void)size;
    // Nothing derives from Node, so every one is this size.
    Carver& c = carver;
    if (!c.block || c.used + kSlot > NodeBlock::kBytes) {
        void* raw = std::malloc(NodeBlock::kBytes);
        if (!raw)
            throw std::bad_alloc();
        // The old block is let go of as a carver; its nodes keep it until they go.
        if (c.block)
            c.retire();
        c.block = new (raw) NodeBlock();
        c.used = kFirst;
        c.carved = 0;
    }
    char* slot = reinterpret_cast<char*>(c.block) + c.used;
    c.used += kSlot;
    ++c.carved;
    *reinterpret_cast<NodeBlock**>(slot) = c.block;
    return slot + kHeader;
}

void Node::operator delete(void* p) noexcept
{
#if defined(__SANITIZE_ADDRESS__)
    ::operator delete(p);
    return;
#endif
    if (p)
        release(*reinterpret_cast<NodeBlock**>(static_cast<char*>(p) - kHeader));
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
