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
        // P69 L2 (lane `lm`): a composite's row (it carries `fieldChildren`) names a
        // TAG — the analyzer's own gate for `SymbolNamespace::Tag`.
        row.namesATag = decl.fieldChildren.has_value();
        if (decl.specifierPrefixRule.has_value()) {
            row.specifierPrefixRule = *decl.specifierPrefixRule;
        }
        // P69 L2a2 (lane `lm`): the body that makes the row a function definition — the
        // analyzer's own discriminator (`declNodeIsFunctionDefinition`).
        if (decl.kindByChild.has_value()
            && decl.kindByChild->whenKind == DeclarationKind::Function) {
            row.definitionBodyRule = decl.kindByChild->whenRule;
            if (row.definitionBodyRule.valid()) definitionBodyRules_.insert(row.definitionBodyRule.v);
        }
        byRule_.emplace(decl.rule.v, row);
    }
    // P68 round 9 (lane `cs`): a field row whose composite LIFTS its names into
    // the enclosing scope (`fieldChildren.liftToEnclosingScope` — C's enumeration
    // constants) is marked, so `record` binds those names where the analyzer
    // republishes them rather than inside the composite body, where they would
    // die with it and read as Unknown — which `sizeof(E)` then committed as a
    // TYPE. The row is found through the SAME config link the analyzer follows.
    // P69 L2 (lane `lm`): a field row whose composite does NOT republish its names
    // declares MEMBERS — the structure's own name space (C 6.2.3), which the sketch
    // never records. Found through the same config link.
    for (auto const& decl : sem.declarations) {
        if (!decl.fieldChildren.has_value() || !decl.fieldChildren->rule.valid())
            continue;
        auto it = byRule_.find(decl.fieldChildren->rule.v);
        if (it == byRule_.end()) continue;
        if (decl.fieldChildren->liftToEnclosingScope) {
            it->second.liftToEnclosingScope = true;
        } else {
            it->second.namesAMember = true;
        }
    }
    for (auto const& [rule, row] : byRule_) {
        if (row.liftToEnclosingScope && row.namesAMember) {
            // One field row shared by a composite that lifts its names and one that
            // does not would make its names both ordinary and members: a config the
            // sketch cannot mirror, stopped rather than guessed at.
            bsFatal("a field row is both lifted and a member row");
        }
    }
    for (auto const& sc : sem.scopes) {
        if (sc.rule.valid()) scopeRules_.insert(sc.rule.v);
    }
    // P69 L2a2 (lane `lm`): a parameter list is a scope — function prototype scope (C
    // 6.2.1p4) — exactly where the analyzer opens one (`prototypeParamScopeRule`).
    if (sem.declarators.has_value() && sem.declarators->prototypeParamScopeRule.has_value()
        && sem.declarators->prototypeParamScopeRule->valid()) {
        protoParamScopeRule_ = *sem.declarators->prototypeParamScopeRule;
        scopeRules_.insert(protoParamScopeRule_.v);
    }
    // A scope rule that ALSO carries a non-Type `declarations` row opens a
    // DECLARATOR-DOMINATOR scope (e.g. c's topLevelDecl — a variable/
    // function declaration that opens a scope only to dominate its params).
    // A composite-TYPE-body scope rule (c25's unified structSpec, isType) is NOT
    // a dominator (its tag is recorded for it, then floats past any enclosing
    // dominator). `block` and friends carry no `declarations` row at all.
    for (auto rv : scopeRules_) {
        auto it = byRule_.find(rv);
        if (it == byRule_.end()) continue;
        if (!it->second.isType) {
            dominatorScopeRules_.insert(rv);
        } else {
            compositeScopeRules_.insert(rv);   // a composite BODY scope
        }
    }
    // The global scope, id 0, never popped; a namespace scope.
    liveScopes_.push(LiveScope{.id = 0, .dominator = false, .composite = false});
    // P68 round 9 (lane `cs`): the language's PREDEFINED identifiers — C 6.4.2.2's
    // `__func__` and the GNU `__FUNCTION__` alias, from the config's
    // `semantics.predefinedFunctionNames` — are ORDINARY identifiers the source
    // never declares, so without an entry they read as Unknown and `sizeof(__func__)`
    // committed as a type. Recorded as VALUES in the global scope: wherever such a
    // name is visible at all it names an object, and where the analyzer does not
    // bind it (outside a function body) the value reading is refused there as an
    // undeclared identifier, which is the right refusal. A later declaration of the
    // same spelling shadows the seed like any other binding.
    for (std::string const& spelling : sem.predefinedFunctionNameIdentifiers) {
        if (spelling.empty()) continue;
        bindings_.push_back(Binding{
            .name   = spelling,
            .scope  = 0,
            .isType = false,
        });
    }
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
    std::uint32_t const id = nextScopeId_++;
    if (liveScopes_.back().id >= id) {   // the ascending order `lookup` reads (see liveScopes_)
        bsFatal("openScope: a scope id at or below a live one - the live stack's order broke");
    }
    liveScopes_.push(LiveScope{.id        = id,
                               .dominator = dominatorScopeRules_.contains(rule.v),
                               .composite = compositeScopeRules_.contains(rule.v)});
}

std::uint32_t BinderSketch::closeScope() {
    // The global scope (slot 0) must never pop — scope events come from
    // the parser's frame open/close of `semantics.scopes` rules, which
    // are balanced by construction; underflow means the caller's event
    // wiring broke. Fail loud, never silently mis-scope bindings.
    if (liveScopes_.size() <= 1) {
        bsFatal("closeScope underflow — scope events out of balance");
    }
    std::uint32_t const closed = liveScopes_.back().id;
    liveScopes_.pop();
    version_ = ++versionSource_;   // the bindings it held are hidden now
    return closed;
}

bool BinderSketch::isPrototypeParamScopeRule(RuleId rule) const noexcept {
    return protoParamScopeRule_.valid() && rule == protoParamScopeRule_;
}

bool BinderSketch::isDefinitionBodyRule(RuleId rule) const noexcept {
    return definitionBodyRules_.contains(rule.v);
}

void BinderSketch::notePrototypeParamScope(std::uint32_t scope, NodeId paramList) {
    closedParamScopes_.push(ClosedParamScope{.scope = scope, .list = paramList});
}

void BinderSketch::keepDefinitionParamsOf(NodeId paramList) {
    definitionParamScope_ = 0;
    if (paramList.valid()) {
        for (ClosedParamScope const c : closedParamScopes_) {
            if (c.list.v == paramList.v) definitionParamScope_ = c.scope;
        }
    }
    closedParamScopes_.truncate(0);
}

void BinderSketch::bindDefinitionParams() {
    std::uint32_t const from = std::exchange(definitionParamScope_, 0);
    if (from == 0) return;
    // Its names, in their order, into the innermost live scope — where a definition's
    // parameters were always bound (c's topLevelDecl scope, which the body is inside). The
    // bindings are appended, never moved, so a restore's truncation stays exact.
    std::size_t const n = bindings_.size();
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (bindings_[i].scope != from) continue;
        Binding copy = bindings_[i];
        copy.scope = liveScopes_.back().id;
        bindings_.push_back(std::move(copy));
        any = true;
    }
    if (any) version_ = ++versionSource_;
}

void BinderSketch::dropDefinitionParams() noexcept {
    definitionParamScope_ = 0;
    closedParamScopes_.truncate(0);
}

void BinderSketch::record(std::string name, bool isType, SourceSpan span,
                          bool liftToEnclosingScope) {
    if (name.empty()) return;   // anonymous/malformed decl — nothing to bind
    // P68 round 9 (lane `cs`): a LIFTED field name (an enumeration constant) is an
    // ordinary identifier of the scope the composite specifier appears in — C
    // 6.2.1p7 — so it binds in the nearest scope that is neither a composite BODY
    // (its own enum, or a struct it is nested in) nor a declarator-dominator: the
    // block or the file, as the analyzer's republication does. The global scope
    // (slot 0) is neither, so the search always lands.
    if (liftToEnclosingScope) {
        std::uint32_t target = liveScopes_[0].id;
        for (std::size_t i = liveScopes_.size(); i-- > 0;) {
            LiveScope const sc = liveScopes_[i];
            if (!sc.dominator && !sc.composite) {
                target = sc.id;
                break;
            }
        }
        bindings_.push_back(Binding{
            .name   = std::move(name),
            .scope  = target,
            .isType = isType,
            .span   = span,
        });
        version_ = ++versionSource_;
        return;
    }
    // A composite/typedef TYPE tag (C11 6.2.1) belongs to the nearest enclosing
    // NAMESPACE scope (block or file), not an interior declarator-dominator
    // scope it may have been minted inside (a file-scope `struct P { … } v;`
    // tag is recorded as the unified structSpec's frame closes — WHILE the
    // enclosing topLevelDecl param-dominator scope is still live — and must
    // bind at file scope so it is visible to the next declaration / exported in
    // globalTypeNames). Float TYPE bindings past dominator scopes; VALUE
    // bindings (params, locals) stay in the live scope. Mirrors the analyzer's
    // floatToNamespaceScope.
    std::uint32_t scope = liveScopes_.back().id;
    if (isType) {
        for (std::size_t i = liveScopes_.size(); i-- > 0;) {
            if (!liveScopes_[i].dominator) { scope = liveScopes_[i].id; break; }
        }
    }
    bindings_.push_back(Binding{
        .name      = std::move(name),
        .scope     = scope,
        .isType    = isType,
        .span      = span,
    });
    version_ = ++versionSource_;
}

void BinderSketch::recordAtDeclaratorEnd(std::string name, bool isType, SourceSpan span,
                                         bool rowOwnsInnermostScope) {
    if (name.empty()) return;   // abstract / malformed — nothing to bind
    // The row's own scope is the innermost live one while its declarators close;
    // its close-time record runs after that scope closed, so this one starts one
    // scope further out. The global scope (slot 0) is never a row's own.
    std::size_t from = liveScopes_.size() - 1;
    if (rowOwnsInnermostScope) {
        if (from == 0) bsFatal("recordAtDeclaratorEnd: the row owns no live scope");
        --from;
    }
    // Then as `record` places a name: a TYPE floats past declarator-dominator
    // scopes, a VALUE stays where it is.
    std::uint32_t scope = liveScopes_[from].id;
    if (isType) {
        for (std::size_t i = from + 1; i-- > 0;) {
            if (!liveScopes_[i].dominator) { scope = liveScopes_[i].id; break; }
        }
    }
    bindings_.push_back(Binding{
        .name   = std::move(name),
        .scope  = scope,
        .isType = isType,
        .span   = span,
    });
    version_ = ++versionSource_;
}

void BinderSketch::recordUnlessBoundAtItsDeclarator(std::string name, bool isType,
                                                    SourceSpan span) {
    // Everything recorded after a name's declarator-end binding comes from tokens
    // after that declarator (the rest of the declaration and whatever it holds), so
    // the backward scan stops at the first binding that starts before this name:
    // it reads only this declaration's own bindings.
    for (auto it = bindings_.rbegin();
         it != bindings_.rend() && it->span.start() >= span.start(); ++it) {
        if (it->span == span && it->name == name) {
            return;   // bound at the end of its declarator already
        }
    }
    record(std::move(name), isType, span);
}

BinderSketch::NameKind BinderSketch::lookup(std::string_view name) const noexcept {
    // Newest-first: the most recent binding in a LIVE scope wins —
    // that's lexical shadowing (an inner `int T;` recorded after a
    // file-scope `typedef ... T` is found first). Only ordinary identifiers are
    // recorded (C 6.2.3), so a tag of the same spelling, however new, is never the
    // answer: `int g(int); struct g {…}; (g)(2)` stays a call.
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
        ++work_;   // a binding visited
        if (it->name != name) continue;
        if (!scopeIsLive_(it->scope)) {
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
    version_ = ++versionSource_;
}

std::vector<std::string> BinderSketch::globalTypeNames() const {
    std::vector<std::string> out;
    for (auto const& b : bindings_) {
        if (b.scope == 0 && b.isType) {
            out.push_back(b.name);
        }
    }
    return out;
}

std::vector<std::pair<std::string, SourceSpan>>
BinderSketch::globalTypeBindings() const {
    std::vector<std::pair<std::string, SourceSpan>> out;
    for (auto const& b : bindings_) {
        if (b.scope == 0 && b.isType) {
            out.emplace_back(b.name, b.span);
        }
    }
    return out;
}

void BinderSketch::recordCandidate(AmbiguousTypeNameCandidate c) {
    candidates_.push_back(std::move(c));
}

std::vector<AmbiguousTypeNameCandidate> BinderSketch::takeCandidates() {
    return std::exchange(candidates_, {});
}

// P69 L2a2 (lane `lm`, the L2a review's N10): ids ascend from the bottom of the live stack (see
// `liveScopes_`), so liveness is a binary search - not a scan of the stack for every binding a
// lookup passes, which a deep nest of scopes would pay per binding.
bool BinderSketch::scopeIsLive_(std::uint32_t id) const noexcept {
    std::span<LiveScope const> const live = liveScopes_.span();
    std::size_t lo = 0;
    std::size_t hi = live.size();
    while (lo < hi) {   // the first entry whose id is not below `id`
        ++work_;        // a live-scope entry visited
        std::size_t const mid = lo + (hi - lo) / 2;
        if (live[mid].id < id) lo = mid + 1;
        else                   hi = mid;
    }
    return lo < live.size() && live[lo].id == id;
}

BinderSketch::Snapshot BinderSketch::snapshot() {
    Snapshot s;
    s.owner_         = this;
    std::size_t const copied = 0;   // entries this snapshot copies: none, it marks
    work_ += 2 + copied;            // one per stack marked, and what it copies
    s.bindingCount   = bindings_.size();
    s.candidateCount = candidates_.size();
    TrailedStack<LiveScope>::Mark const live = liveScopes_.mark();
    s.liveScopesMarkSize    = live.size;
    s.liveScopesMarkJournal = live.journal;
    TrailedStack<ClosedParamScope>::Mark const closed = closedParamScopes_.mark();
    s.closedParamsMarkSize    = closed.size;
    s.closedParamsMarkJournal = closed.journal;
    s.nextScopeId          = nextScopeId_;
    s.version              = version_;
    s.definitionParamScope = definitionParamScope_;
    ++liveMarks_;
    return s;
}

BinderSketch::Snapshot::Snapshot(Snapshot&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr))
    , bindingCount(other.bindingCount)
    , candidateCount(other.candidateCount)
    , liveScopesMarkSize(other.liveScopesMarkSize)
    , liveScopesMarkJournal(other.liveScopesMarkJournal)
    , closedParamsMarkSize(other.closedParamsMarkSize)
    , closedParamsMarkJournal(other.closedParamsMarkJournal)
    , nextScopeId(other.nextScopeId)
    , version(other.version)
    , definitionParamScope(other.definitionParamScope) {}

BinderSketch::Snapshot::~Snapshot() {
    if (owner_ != nullptr) owner_->releaseMark_();
}

void BinderSketch::releaseMark_() noexcept {
    if (liveMarks_ == 0) bsFatal("a snapshot released more often than taken");
    if (--liveMarks_ == 0) {
        liveScopes_.discardJournal();
        closedParamScopes_.discardJournal();
    }
}

void BinderSketch::restore(Snapshot&& s) {
    if (s.owner_ != this) {
        bsFatal("restore with a snapshot of another sketch, or one moved from");
    }
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
    // The restore's work: the journal records it replays, and one.
    work_ += 1 + (liveScopes_.journalSize() - std::min(liveScopes_.journalSize(), s.liveScopesMarkJournal))
           + (closedParamScopes_.journalSize()
              - std::min(closedParamScopes_.journalSize(), s.closedParamsMarkJournal));
    liveScopes_.rewindTo(TrailedStack<LiveScope>::Mark{.size    = s.liveScopesMarkSize,
                                                        .journal = s.liveScopesMarkJournal});
    closedParamScopes_.rewindTo(TrailedStack<ClosedParamScope>::Mark{
        .size = s.closedParamsMarkSize, .journal = s.closedParamsMarkJournal});
    nextScopeId_          = s.nextScopeId;
    version_              = s.version;   // `versionSource_` stays: a number is never given twice
    definitionParamScope_ = s.definitionParamScope;
}

} // namespace dss
