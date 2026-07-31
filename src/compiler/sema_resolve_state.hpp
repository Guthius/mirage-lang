#pragma once

// The resolve-phase reentrancy state, split out of sema.hpp: the cycle-guard
// sets/stacks Program carries while lazily resolving types, values and
// signatures out of declaration order, plus the RAII guard types that keep
// them balanced on every exit path.

#include "resolved_type.hpp"
#include "sema_generics.hpp"

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sema {
    struct ResolveState {
        std::set<std::pair<std::string, std::string>> alias_resolving;
        std::set<std::pair<std::string, std::string>> struct_resolving;
        std::set<std::pair<std::string, std::string>> union_resolving;
        std::set<std::pair<std::string, std::string>> bitset_resolving;
        std::set<std::pair<std::string, std::string>> trait_resolving;
        std::set<std::pair<std::string, std::string>> value_resolving;
        std::set<std::pair<std::string, std::string>> fn_signature_resolving;
        // Cycle guard for the reentrant module-scope-'when' symbol declaration helper
        // (ensure_module_declared, sema_declare.cpp) — a 'when' condition that references
        // another module's const can force that module's symbol table to be built
        // on-demand, out of Program::modules' unordered iteration order; this detects a
        // circular dependency between modules' 'when' conditions.
        std::set<std::string> when_module_declaring;
        // One in-flight instantiate_generic_type call: its (decl, concrete args) key, plus
        // the slot it pre-allocated for the result before resolving the declaration's RHS.
        struct GenericTypeInProgress {
            GenericInstanceKey key;
            // The already-allocated, NOT-yet-laid-out handle, when the declaration's RHS is
            // an aggregate. Re-entering with the same key returns this instead of erroring,
            // which is what lets 'type Node[T: type] = struct { next: *Node[T] }' work: the
            // pointer field only needs the handle, never the layout.
            //
            // nullopt when the RHS is NOT an aggregate ('type Ptr[T: type] = *T'), where
            // there is no slot to hand back and self-reference really is an infinite
            // regress. Those still report a cycle at instantiation time.
            std::optional<ResolvedType> slot;
        };
        // In-flight generic type instantiations, innermost last. Mirrors what the non-generic
        // path gets for free from TypeSymbol::resolved being populated at declare time (see
        // resolve_final_shallow's early return, which is exactly why '*block_header' works
        // and '*Node[T]' did not until this existed).
        //
        // By-value cycles are NOT detected here — a slot handed back is indistinguishable
        // from a finished one at this point. They are caught at LAYOUT time instead, by
        // layout_struct/layout_union rejecting a field whose type is an aggregate still
        // missing its layout_done. Linear-scan (small, rare), not a set, since
        // GenericInstanceKey has no operator<.
        std::vector<GenericTypeInProgress> generic_type_resolving;
        // Ordered ancestor stack of trait_index currently mid-flattening (layout_trait's
        // composition-resolution step, type_resolver.cpp) — pushed/popped around that step
        // only. Distinct from trait_resolving above: that set exists purely to short-circuit
        // a trait referencing ITSELF in a METHOD SIGNATURE (not a true cycle, since a trait
        // handle is a fixed-size fat pointer); this stack instead detects a genuine trait
        // COMPOSITION cycle ('trait(A)' -> ... -> 'trait(A)'), which must be a hard error
        // naming the full chain.
        std::vector<int> trait_composition_stack;
    };

    // RAII helpers for the cycle guards above.
    //
    // Every guard used to be a hand-written insert(key) ... erase(key) (or push_back/pop_back)
    // pair spanning a large recursive body. Any exception thrown in between -- an .at() miss,
    // a bad std::get, an uncaught std::stoll -- left the key permanently marked "resolving",
    // so every later attempt to resolve that type reported a spurious "circular dependency".
    // (The ambient pointer stacks had it worse still — see AmbientScopeStack in
    // sema_generics.hpp, which now enforces pairing for those by construction.)
    //
    // These make the unwind path correct by construction. lsp/server.cpp runs sema on a
    // worker thread with no try/catch anywhere above it, so a poisoned guard would otherwise
    // persist for the lifetime of the editor session.
    template <typename Set, typename Key>
    class ScopedResolveMark {
      public:
        ScopedResolveMark(Set &set, Key key) : set_(set), key_(std::move(key)) { set_.insert(key_); }
        ~ScopedResolveMark() { set_.erase(key_); }

        ScopedResolveMark(const ScopedResolveMark &) = delete;
        auto operator=(const ScopedResolveMark &) -> ScopedResolveMark & = delete;

      private:
        Set &set_;
        Key key_;
    };

    template <typename Set, typename Key>
    ScopedResolveMark(Set &, Key) -> ScopedResolveMark<Set, Key>;

    template <typename Stack>
    class ScopedResolvePush {
      public:
        template <typename Value>
        ScopedResolvePush(Stack &stack, Value &&value) : stack_(stack) {
            stack_.push_back(std::forward<Value>(value));
        }
        ~ScopedResolvePush() { stack_.pop_back(); }

        ScopedResolvePush(const ScopedResolvePush &) = delete;
        auto operator=(const ScopedResolvePush &) -> ScopedResolvePush & = delete;

      private:
        Stack &stack_;
    };

    template <typename Stack, typename Value>
    ScopedResolvePush(Stack &, Value &&) -> ScopedResolvePush<Stack>;
}
