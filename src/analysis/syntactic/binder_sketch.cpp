#include "analysis/syntactic/binder_sketch.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace dss {

namespace {

[[noreturn]] void bsFatal(char const* what) noexcept {
    std::fputs("dss::BinderSketch fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

BinderSketch::BinderSketch(GrammarSchema const& schema) {
    SemanticConfig const& sem = schema.semantics();
    for (auto const& decl : sem.declarations) {
        // Only name-bearing rows participate — a declaration that binds
        // nothing gives the triage nothing to look up. Two name-bearing
        // shapes exist: the legacy positional `nameChild`, and (FC4 c1)
        // declarator-mode rows whose names the declarator walk extracts
        // below the list/single carrier child. The loader guarantees a
        // declarator-mode row's language declares the `declarators` block.
        if (!decl.rule.valid()) continue;
        BinderDecl row;
        if (decl.nameChild.has_value()) {
            row.nameChild = *decl.nameChild;
        } else if (sem.declarators.has_value()
                   && (decl.declaratorListChild.has_value()
                       || decl.declaratorChild.has_value())) {
            row.declaratorMode = true;
            row.carrierChild   = decl.declaratorListChild.has_value()
                                     ? *decl.declaratorListChild
                                     : *decl.declaratorChild;
        } else {
            continue;
        }
        // STATIC kind only. `kindByChild` discriminators flip
        // Variable→Function — both VALUES to the type-name triage — so
        // evaluating the discriminator here would buy nothing. A future
        // language whose discriminator can flip a row to Type extends
        // this (the analyzer's evaluation is the template).
        row.isType    = decl.kind == DeclarationKind::Type;
        row.nameMatch = decl.nameMatch;
        if (decl.specifierPrefixRule.has_value()) {
            row.specifierPrefixRule = *decl.specifierPrefixRule;
        }
        byRule_.emplace(decl.rule.v, row);
    }
    for (auto const& sc : sem.scopes) {
        if (sc.rule.valid()) scopeRules_.insert(sc.rule.v);
    }
    // A scope rule that ALSO carries a non-Type `declarations` row opens a
    // DECLARATOR-DOMINATOR scope (e.g. c-subset's topLevelDecl — a variable/
    // function declaration that opens a scope only to dominate its params).
    // A composite-TYPE-body scope rule (structSpecifierBody, isType) is NOT a
    // dominator (its tag is recorded for it, then floats past any enclosing
    // dominator). `block` and friends carry no `declarations` row at all.
    for (auto rv : scopeRules_) {
        auto it = byRule_.find(rv);
        if (it != byRule_.end() && !it->second.isType) {
            dominatorScopeRules_.insert(rv);
        }
    }
    liveScopes_.push_back(0);   // the global scope, id 0, never popped
    liveScopeIsDominator_.push_back(false);   // global is a namespace scope
}

BinderSketch::BinderDecl const*
BinderSketch::binderFor(RuleId rule) const noexcept {
    auto it = byRule_.find(rule.v);
    return it == byRule_.end() ? nullptr : &it->second;
}

bool BinderSketch::isScopeRule(RuleId rule) const noexcept {
    return scopeRules_.contains(rule.v);
}

void BinderSketch::openScope(RuleId rule) {
    liveScopes_.push_back(nextScopeId_++);
    liveScopeIsDominator_.push_back(dominatorScopeRules_.contains(rule.v));
}

void BinderSketch::closeScope() {
    // The global scope (slot 0) must never pop — scope events come from
    // the parser's frame open/close of `semantics.scopes` rules, which
    // are balanced by construction; underflow means the caller's event
    // wiring broke. Fail loud, never silently mis-scope bindings.
    if (liveScopes_.size() <= 1) {
        bsFatal("closeScope underflow — scope events out of balance");
    }
    liveScopes_.pop_back();
    liveScopeIsDominator_.pop_back();
}

void BinderSketch::record(std::string name, bool isType) {
    if (name.empty()) return;   // anonymous/malformed decl — nothing to bind
    // A composite/typedef TYPE tag (C11 6.2.1) belongs to the nearest enclosing
    // NAMESPACE scope (block or file), not an interior declarator-dominator
    // scope it may have been minted inside (a file-scope `struct P { … } v;`
    // tag is recorded as structSpecifierBody's frame closes — WHILE the
    // enclosing topLevelDecl param-dominator scope is still live — and must
    // bind at file scope so it is visible to the next declaration / exported in
    // globalTypeNames). Float TYPE bindings past dominator scopes; VALUE
    // bindings (params, locals) stay in the live scope. Mirrors the analyzer's
    // floatToNamespaceScope.
    std::uint32_t scope = liveScopes_.back();
    if (isType) {
        for (std::size_t i = liveScopes_.size(); i-- > 0;) {
            if (!liveScopeIsDominator_[i]) { scope = liveScopes_[i]; break; }
        }
    }
    bindings_.push_back(Binding{
        .name   = std::move(name),
        .scope  = scope,
        .isType = isType,
    });
}

BinderSketch::NameKind BinderSketch::lookup(std::string_view name) const noexcept {
    // Newest-first: the most recent binding in a LIVE scope wins —
    // that's lexical shadowing (an inner `int T;` recorded after a
    // file-scope `typedef ... T` is found first).
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
        if (it->name != name) continue;
        if (std::ranges::find(liveScopes_, it->scope) == liveScopes_.end()) {
            continue;   // out-of-scope binding — dead, skip
        }
        return it->isType ? NameKind::Type : NameKind::Value;
    }
    return NameKind::Unknown;
}

void BinderSketch::seedGlobalType(std::string name) {
    if (name.empty()) return;
    bindings_.push_back(Binding{
        .name   = std::move(name),
        .scope  = 0,
        .isType = true,
    });
}

std::vector<std::string> BinderSketch::globalTypeNames() const {
    std::vector<std::string> out;
    for (auto const& b : bindings_) {
        if (b.scope == 0 && b.isType) out.push_back(b.name);
    }
    return out;
}

void BinderSketch::recordCandidate(AmbiguousTypeNameCandidate c) {
    candidates_.push_back(std::move(c));
}

std::vector<AmbiguousTypeNameCandidate> BinderSketch::takeCandidates() {
    return std::exchange(candidates_, {});
}

BinderSketch::Snapshot BinderSketch::snapshot() const {
    return Snapshot{
        .bindingCount       = bindings_.size(),
        .candidateCount     = candidates_.size(),
        .liveScopes         = liveScopes_,
        .liveScopeDominator = liveScopeIsDominator_,
        .nextScopeId        = nextScopeId_,
    };
}

void BinderSketch::restore(Snapshot&& s) {
    // Bindings/candidates are append-only between snapshot and restore
    // (closeScope never truncates), so truncate-to-count is exact.
    if (s.bindingCount > bindings_.size()
        || s.candidateCount > candidates_.size()) {
        bsFatal("restore with a snapshot newer than current state");
    }
    bindings_.resize(s.bindingCount);
    // erase (not resize): AmbiguousTypeNameCandidate is not default-
    // constructible (SourceSpan has factory-only construction), and
    // resize requires DefaultInsertable even when shrinking.
    candidates_.erase(
        candidates_.begin() + static_cast<std::ptrdiff_t>(s.candidateCount),
        candidates_.end());
    liveScopes_           = std::move(s.liveScopes);
    liveScopeIsDominator_ = std::move(s.liveScopeDominator);
    nextScopeId_          = s.nextScopeId;
}

} // namespace dss
