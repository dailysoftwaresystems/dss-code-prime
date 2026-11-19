#pragma once

#include "core/types/scope_kind.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace dss {

// ★ P69 L2b2 (lane `lm`): A SCOPE STACK IS HANDED OUT IN O(1), AND ITS KINDS ARE READ ONLY WHEN
// THEY ARE RENDERED OR ASKED FOR. Every diagnostic carries the scope stack it was raised under
// (rendered `scope: Root > Block > Paren`), and the tree builder used to COPY its whole stack into
// each one. A refuted alternative of a speculation probe raises its expected-token diagnostics
// inside the checkpoint, and the rollback drops them — so a nest that probes at every level copied
// a stack as deep as the nest once per diagnostic: Θ(depth²) per nest. ✔MEASURED (the cost file's
// probe nests climbed a doubling at a time, macos-arm64-debug): the builder's scope work multiplied
// by x3.14–x3.53 per doubling at 125 → 250 levels, a whole read of the stack a fixed number of
// times per level.
//
// A trail is a HANDLE: the chain its stack was grown in, the node of its top, that node's push
// serial and its depth. A trail stays exactly what it was whatever the stack does after it was
// taken — a kept diagnostic renders the stack of the moment it was raised — because its owner
// drops a node only when NO trail can read it (P69 L2b2, lane `lm`: the tree builder drops a
// rolled-back branch's nodes and a popped scope's node that no trail handed out reads; it held one
// node per scope ever pushed, 149,187 on sqlite3.c, where the stack it mirrors is as deep as the
// nesting). A dropped node's id is taken again by the next push, so every node carries the SERIAL
// of the push that made it — never reused — and a trail checks it at every link it reads: a trail
// that outlived its nodes is FATAL, never a rendering of another scope's kinds. `kinds()`
// materialises it, a loop down the links (never recursion), O(depth) — paid by a rendering or a
// question, never by the capture.

// The nodes trails are read from: one per scope on the stack it mirrors and per scope a trail
// handed out still reads, each naming its kind, the node below it and the serials of both. Grown
// and shrunk by ONE owner (the tree builder), read through the trails it hands out, on the thread
// that grows it until the owner is done.
class ScopeChain {
public:
    static constexpr std::uint32_t kNoNode = std::numeric_limits<std::uint32_t>::max();

    // Append a node of `kind` above node `below` (`kNoNode`: the bottom of a stack); its id. The
    // node takes the next push serial, and records the serial of the node below it.
    [[nodiscard]] std::uint32_t push(ScopeKind kind, std::uint32_t below) {
        if (below != kNoNode && below >= nodes_.size()) {
            fail("ScopeChain::push above a node the chain does not hold");
        }
        if (nodes_.size() >= kNoNode) fail("ScopeChain::push past the chain's node ids");
        std::uint64_t const belowSerial = below == kNoNode ? 0 : nodes_[below].serial;
        nodes_.push_back(Node{below, kind, ++lastSerial_, belowSerial});
        return static_cast<std::uint32_t>(nodes_.size() - 1);
    }

    // P69 L2b2 (lane `lm`): drop every node at or above `size` — a rollback's, when no trail can
    // read them (the owner's argument). A larger `size` is a caller defect: fatal.
    void truncateTo(std::size_t size) noexcept {
        if (size > nodes_.size()) fail("ScopeChain::truncateTo above the chain's size");
        nodes_.resize(size);
    }
    // P69 L2b2 (lane `lm`): drop the newest node — a pop's, when no trail can read it. The next
    // push takes its id again, under a new serial.
    void dropNewest() noexcept {
        if (nodes_.empty()) fail("ScopeChain::dropNewest on an empty chain");
        nodes_.pop_back();
    }

    // The nodes the chain holds now: never more than one per push, never fewer than its owner's
    // stack and the trails handed out can read.
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    [[noreturn]] static void fail(char const* what) noexcept {
        std::fprintf(stderr, "scope trail fatal: %s\n", what);
        std::abort();
    }

private:
    friend class ScopeTrail;
    friend class ScopeChainCompaction;

    struct Node {
        std::uint32_t below;
        ScopeKind     kind;
        std::uint64_t serial;        // the push that made it, never reused (P69 L2b2)
        std::uint64_t belowSerial;   // `below`'s serial when this node was pushed (0: none)
    };

    [[nodiscard]] Node const& node(std::uint32_t id) const noexcept {
        if (id >= nodes_.size()) fail("a scope trail links to a node its chain does not hold");
        return nodes_[id];
    }
    // The node a link names, checked against the serial the link recorded: a node reused after the
    // link was made is FATAL — the trail outlived the node it read.
    [[nodiscard]] Node const& linked(std::uint32_t id, std::uint64_t serial) const noexcept {
        Node const& n = node(id);
        if (n.serial != serial) {
            fail("a scope trail reads a node reused after the trail was taken (its scope was dropped "
                 "while the trail could still read it)");
        }
        return n;
    }

    std::vector<Node> nodes_;
    std::uint64_t     lastSerial_ = 0;
};

class ScopeTrail {
public:
    ScopeTrail() noexcept = default;

    // A stack written out by its holder, bottom first (an ingested diagnostic's own, a test's): the
    // trail grows a chain of its own.
    ScopeTrail(std::initializer_list<ScopeKind> kinds)
        : ScopeTrail(std::span<ScopeKind const>{kinds.begin(), kinds.size()}) {}
    explicit ScopeTrail(std::span<ScopeKind const> kinds) {
        if (kinds.empty()) return;
        auto          chain = std::make_shared<ScopeChain>();
        std::uint32_t top   = ScopeChain::kNoNode;
        for (ScopeKind const k : kinds) top = chain->push(k, top);
        headSerial_ = chain->node(top).serial;
        chain_      = std::move(chain);
        head_       = top;
        depth_      = static_cast<std::uint32_t>(kinds.size());
    }

    // The stack whose top is node `head` of `chain`, `depth` scopes deep — O(1): a handle, never a
    // copy. Depth 0 is the empty stack whatever `head` says.
    [[nodiscard]] static ScopeTrail at(std::shared_ptr<ScopeChain const> chain, std::uint32_t head,
                                       std::uint32_t depth) noexcept {
        ScopeTrail t;
        if (depth == 0) return t;
        if (!chain) ScopeChain::fail("a scope trail of a non-empty stack names no chain");
        t.headSerial_ = chain->node(head).serial;
        t.chain_      = std::move(chain);
        t.head_       = head;
        t.depth_      = depth;
        return t;
    }

    [[nodiscard]] bool        empty() const noexcept { return depth_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return depth_; }

    // The kinds, bottom first: the `depth` scopes ending at its top. O(depth), a loop down the
    // links. A trail whose links end before its depth is corrupt — fatal, never a guess.
    [[nodiscard]] std::vector<ScopeKind> kinds() const {
        std::vector<ScopeKind> out(depth_);
        std::uint32_t          at     = head_;
        std::uint64_t          serial = headSerial_;
        for (std::size_t i = depth_; i-- > 0;) {
            if (!chain_ || at == ScopeChain::kNoNode) {
                ScopeChain::fail("a scope trail's links end before its depth");
            }
            ScopeChain::Node const& n = chain_->linked(at, serial);
            out[i]                    = n.kind;
            at                        = n.below;
            serial                    = n.belowSerial;
        }
        return out;
    }

    // The same handle — the same chain, top, top serial and depth — not merely the same kinds.
    [[nodiscard]] bool isSameTrail(ScopeTrail const& o) const noexcept {
        return chain_ == o.chain_ && head_ == o.head_ && headSerial_ == o.headSerial_
            && depth_ == o.depth_;
    }

    // The nodes the trail's chain holds, its own and every other trail's (0: none) — what holding
    // this trail keeps alive.
    [[nodiscard]] std::size_t chainSize() const noexcept { return chain_ ? chain_->size() : 0; }

private:
    friend class ScopeChainCompaction;

    std::shared_ptr<ScopeChain const> chain_;
    std::uint32_t                     head_       = ScopeChain::kNoNode;
    std::uint64_t                     headSerial_ = 0;   // the top's push serial (P69 L2b2)
    std::uint32_t                     depth_      = 0;
};

// Re-homes the trails of ONE chain into a fresh chain holding only the nodes they read, so a
// finished owner's long chain (a node per scope push of a whole parse) is not kept alive by the few
// trails that outlive it. Trails of another chain pass unchanged. Each node is copied once, the
// first time a trail reads it, so trails sharing a bottom share it in the fresh chain too:
// O(the nodes read), every walk a loop.
class ScopeChainCompaction {
public:
    explicit ScopeChainCompaction(std::shared_ptr<ScopeChain const> from)
        : from_(std::move(from))
        , into_(std::make_shared<ScopeChain>())
        , copied_(from_ ? from_->size() : 0, ScopeChain::kNoNode) {}

    [[nodiscard]] ScopeTrail rehome(ScopeTrail const& t) {
        if (t.empty() || !from_ || t.chain_ != from_) return t;
        std::vector<std::uint32_t> uncopied;   // the trail's nodes not yet copied, top first
        std::uint32_t              at     = t.head_;
        std::uint64_t              serial = t.headSerial_;
        std::uint32_t              steps  = 0;
        while (steps < t.depth_) {
            if (at >= copied_.size()) {
                ScopeChain::fail("a scope trail links to a node its chain does not hold");
            }
            ScopeChain::Node const& n = from_->linked(at, serial);   // P69 L2b2: fail loud on reuse
            if (copied_[at] != ScopeChain::kNoNode) break;
            uncopied.push_back(at);
            at     = n.below;
            serial = n.belowSerial;
            ++steps;
        }
        std::uint32_t below = steps < t.depth_ ? copied_[at] : ScopeChain::kNoNode;
        for (auto it = uncopied.rbegin(); it != uncopied.rend(); ++it) {
            below        = into_->push(from_->node(*it).kind, below);
            copied_[*it] = below;
        }
        return ScopeTrail::at(into_, copied_[t.head_], t.depth_);
    }

private:
    std::shared_ptr<ScopeChain const> from_;
    std::shared_ptr<ScopeChain>       into_;
    std::vector<std::uint32_t>        copied_;   // a node of `from_` -> its copy in `into_`
};

} // namespace dss
