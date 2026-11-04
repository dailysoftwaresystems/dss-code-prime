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
    liveScopes_.push(LiveScope{.id = 0, .firstBinding = 0, .dominator = false, .composite = false});
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
        pushBinding_(Binding{
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

// ── P69 L2b1 (lane `lm`, the L2a2 review's B1): the per-name index ──
// A head moves only (a) to a binding just recorded, or (b) past a binding whose scope is closed:
// a closed scope opens again only when a restore rewinds the live stack, and a restore undoes
// every head change made since its snapshot together with the stack. So every binding a head
// has moved past is dead, and the newest LIVE binding of a name is always on its chain at or
// after the head — the answer the old newest-first scan of every binding gave.
void BinderSketch::setHead_(std::uint32_t& head, std::uint32_t to) {
    if (liveMarks_ > 0) headJournal_.push_back(HeadChange{.head = &head, .old = head});
    head = to;
}

void BinderSketch::pushBinding_(Binding b) {
    if (bindings_.size() >= kNoBinding) bsFatal("more bindings than a binding index can name");
    auto const index = static_cast<std::uint32_t>(bindings_.size());
    auto it = heads_.find(std::string_view{b.name});
    if (it == heads_.end()) it = heads_.emplace(b.name, kNoBinding).first;
    // Passed before the new binding links to them, so the head this push journals is already past
    // them and a rollback of the push does not bring them back to be walked again.
    passBindingsDeadForEveryLiveSnapshot_(it->second);
    b.prevSameName = it->second;
    bindings_.push_back(std::move(b));
    setHead_(it->second, index);
}

bool BinderSketch::diedBeforeEveryLiveSnapshot_(Binding const& b,
                                                std::uint32_t index) const noexcept {
    if (liveMarks_ == 0) return true;   // no snapshot to restore: no head change is ever undone
    return index < stableBindingCount_ && b.scope < closedAt_.size()
        && closedAt_[b.scope] <= stableBefore_;
}

void BinderSketch::passBindingsDeadForEveryLiveSnapshot_(std::uint32_t& head) noexcept {
    while (head != kNoBinding) {
        Binding const& b = bindings_[head];
        if (scopeIsLive_(b.scope) || !diedBeforeEveryLiveSnapshot_(b, head)) return;
        ++work_;   // a binding passed, for good
        head = b.prevSameName;   // not journaled: it is dead in every state a restore brings back
    }
}

void BinderSketch::openScope(RuleId rule) {
    std::uint32_t const id = nextScopeId_++;
    if (liveScopes_.back().id >= id) {   // the ascending order `lookup` reads (see liveScopes_)
        bsFatal("openScope: a scope id at or below a live one - the live stack's order broke");
    }
    // Every binding of this scope is recorded from here on (its id did not exist before): where a
    // re-bind of its bindings starts (`bindDefinitionParams`).
    liveScopes_.push(LiveScope{.id           = id,
                               .firstBinding = static_cast<std::uint32_t>(bindings_.size()),
                               .dominator    = dominatorScopeRules_.contains(rule.v),
                               .composite    = compositeScopeRules_.contains(rule.v)});
}

BinderSketch::ClosedScope BinderSketch::closeScope() {
    // The global scope (slot 0) must never pop — scope events come from
    // the parser's frame open/close of `semantics.scopes` rules, which
    // are balanced by construction; underflow means the caller's event
    // wiring broke. Fail loud, never silently mis-scope bindings.
    if (liveScopes_.size() <= 1) {
        bsFatal("closeScope underflow — scope events out of balance");
    }
    LiveScope const closed = liveScopes_.back();
    liveScopes_.pop();
    version_ = ++versionSource_;   // the bindings it held are hidden now
    if (closed.id >= closedAt_.size()) closedAt_.resize(std::size_t{closed.id} + 1, 0);
    closedAt_[closed.id] = version_;   // when: a number never given twice (`versionSource_`)
    return ClosedScope{.id = closed.id, .firstBinding = closed.firstBinding};
}

bool BinderSketch::isPrototypeParamScopeRule(RuleId rule) const noexcept {
    return protoParamScopeRule_.valid() && rule == protoParamScopeRule_;
}

bool BinderSketch::isDefinitionBodyRule(RuleId rule) const noexcept {
    return definitionBodyRules_.contains(rule.v);
}

void BinderSketch::notePrototypeParamScope(ClosedScope scope, NodeId paramList,
                                           std::uint32_t listStart) {
    closedParamScopes_.push(ClosedParamScope{.scope        = scope.id,
                                             .firstBinding = scope.firstBinding,
                                             .listStart    = listStart,
                                             .list         = paramList});
}

// ★ P69 L2b1 (lane `lm`, the L2a2 review's A1): lists nest with their tokens, so the lists closed
// inside a declarator or a row that starts at `start` are exactly the entries noted since it
// started — the top of the stack — and every one of them starts at or after `start`, while every
// entry below them closed before it started and starts before it. Popping while the top starts at
// or after `start` therefore takes exactly the construct's own lists, each entry once.
BinderSketch::ClosedParamScope BinderSketch::consumeParamScopesFrom_(std::uint32_t start,
                                                                     NodeId keep) {
    ClosedParamScope found{};
    while (closedParamScopes_.size() > 0) {
        ClosedParamScope const top = closedParamScopes_.back();
        if (top.listStart < start) break;
        if (keep.valid() && top.list.v == keep.v) found = top;
        closedParamScopes_.pop();
    }
    return found;
}

void BinderSketch::keepDefinitionParamsOf(NodeId paramList, std::uint32_t declaratorStart) {
    ClosedParamScope const kept = consumeParamScopesFrom_(declaratorStart, paramList);
    // Only a declarator that names its own list replaces what is kept: a nested declarator that
    // keeps none (a parameter, a returned type's parameter) leaves the enclosing one's alone.
    if (kept.scope != 0) definitionParams_ = kept;
}

void BinderSketch::bindDefinitionParams() {
    ClosedParamScope const from = std::exchange(definitionParams_, ClosedParamScope{});
    if (from.scope == 0) return;
    // Its names, in their order, into the innermost live scope — where a definition's
    // parameters were always bound (c's topLevelDecl scope, which the body is inside). The
    // bindings are appended, never moved, so a restore's truncation stays exact.
    // P69 L2b1 (lane `lm`, the L2a2 review's A2): from where the list's scope opened, counted —
    // every binding of that scope was recorded after it opened (its id did not exist before), so
    // the bindings before it need no visit. It scanned from the first binding of the file,
    // uncounted: Θ(definitions × bindings) for a file of definitions.
    std::uint32_t const into = liveScopes_.back().id;
    std::size_t const   n    = bindings_.size();
    bool any = false;
    for (std::size_t i = from.firstBinding; i < n; ++i) {
        ++work_;   // a binding visited
        if (bindings_[i].scope != from.scope) continue;
        Binding copy = bindings_[i];
        copy.scope   = into;
        pushBinding_(std::move(copy));
        any = true;
    }
    if (any) version_ = ++versionSource_;
}

void BinderSketch::dropDefinitionParams(std::uint32_t rowStart) noexcept {
    (void)consumeParamScopesFrom_(rowStart, NodeId{});
    if (definitionParams_.scope != 0 && definitionParams_.listStart >= rowStart) {
        definitionParams_ = ClosedParamScope{};
    }
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
        std::uint32_t target = 0;
        for (std::size_t i = liveScopes_.size(); i-- > 0;) {
            LiveScope const sc = liveScopes_[i];
            if (!sc.dominator && !sc.composite) {
                target = sc.id;
                break;
            }
        }
        pushBinding_(Binding{
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
            LiveScope const sc = liveScopes_[i];
            if (!sc.dominator) { scope = sc.id; break; }
        }
    }
    pushBinding_(Binding{
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
            LiveScope const sc = liveScopes_[i];
            if (!sc.dominator) { scope = sc.id; break; }
        }
    }
    (void)boundAtDeclaratorEnd_.insert(span.start());
    pushBinding_(Binding{
        .name   = std::move(name),
        .scope  = scope,
        .isType = isType,
        .span   = span,
    });
    version_ = ++versionSource_;
}

void BinderSketch::recordUnlessBoundAtItsDeclarator(std::string name, bool isType,
                                                    SourceSpan span) {
    // The declarator-end binding noted its declaring token; the note is taken back here, once.
    if (boundAtDeclaratorEnd_.erase(span.start())) return;
    record(std::move(name), isType, span);
}

BinderSketch::NameKind BinderSketch::lookup(std::string_view name) noexcept {
    // The most recent binding in a LIVE scope wins — that's lexical shadowing (an inner
    // `int T;` recorded after a file-scope `typedef ... T` is found first). Only ordinary
    // identifiers are recorded (C 6.2.3), so a tag of the same spelling, however new, is never
    // the answer: `int g(int); struct g {…}; (g)(2)` stays a call. The walk is the name's own
    // chain (see `setHead_`); the dead bindings it meets at the head are moved past, once.
    auto const it = heads_.find(name);
    if (it == heads_.end()) return NameKind::Unknown;
    std::uint32_t& head = it->second;
    passBindingsDeadForEveryLiveSnapshot_(head);
    for (std::uint32_t i = head; i != kNoBinding;) {
        ++work_;   // a binding visited
        Binding const& b = bindings_[i];
        if (scopeIsLive_(b.scope)) {
            if (i != head) setHead_(head, i);
            return b.isType ? NameKind::Type : NameKind::Value;
        }
        i = b.prevSameName;
    }
    if (head != kNoBinding) setHead_(head, kNoBinding);
    return NameKind::Unknown;
}

void BinderSketch::seedGlobalType(std::string name) {
    if (name.empty()) return;
    pushBinding_(Binding{
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
// lookup passes, which a deep nest of scopes would pay per binding. Each entry it reads counts
// (`TrailedStack::reads`).
bool BinderSketch::scopeIsLive_(std::uint32_t id) const noexcept {
    std::size_t lo = 0;
    std::size_t hi = liveScopes_.size();
    while (lo < hi) {   // the first entry whose id is not below `id`
        std::size_t const mid = lo + (hi - lo) / 2;
        if (liveScopes_[mid].id < id) lo = mid + 1;
        else                          hi = mid;
    }
    return lo < liveScopes_.size() && liveScopes_[lo].id == id;
}

BinderSketch::Snapshot BinderSketch::snapshot() {
    Snapshot s;
    s.owner_ = this;
    ++work_;   // a snapshot: it marks every trail and copies nothing (`Snapshot::Fields`)
    TrailedStack<LiveScope>::Mark const        live   = liveScopes_.mark();
    TrailedStack<ClosedParamScope>::Mark const closed = closedParamScopes_.mark();
    s.f = Snapshot::Fields{
        .bindingCount            = bindings_.size(),
        .candidateCount          = candidates_.size(),
        .liveScopesMarkSize      = live.size,
        .liveScopesMarkJournal   = live.journal,
        .closedParamsMarkSize    = closed.size,
        .closedParamsMarkJournal = closed.journal,
        .headJournalSize         = headJournal_.size(),
        .boundAtEndMark          = boundAtDeclaratorEnd_.mark(),
        .nextScopeId             = nextScopeId_,
        .version                 = version_,
        .definitionParams        = definitionParams_,
    };
    if (liveMarks_ == 0) {   // the outermost live snapshot: what is dead now stays dead until it goes
        stableBefore_       = versionSource_;
        stableBindingCount_ = bindings_.size();
    }
    ++liveMarks_;
    return s;
}

BinderSketch::Snapshot::Snapshot(Snapshot&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr))
    , f(other.f) {}

BinderSketch::Snapshot::~Snapshot() {
    if (owner_ != nullptr) owner_->releaseMark_();
}

void BinderSketch::releaseMark_() noexcept {
    if (liveMarks_ == 0) bsFatal("a snapshot released more often than taken");
    if (--liveMarks_ == 0) {
        liveScopes_.discardJournal();
        closedParamScopes_.discardJournal();
        boundAtDeclaratorEnd_.discardJournal();
        headJournal_.clear();
    }
}

void BinderSketch::restore(Snapshot&& s) {
    if (s.owner_ != this) {
        bsFatal("restore with a snapshot of another sketch, or one moved from");
    }
    Snapshot::Fields const& f = s.f;
    // Bindings/candidates are append-only between snapshot and restore
    // (closeScope never truncates), so truncate-to-count is exact.
    if (f.bindingCount > bindings_.size() || f.candidateCount > candidates_.size()
        || f.headJournalSize > headJournal_.size()) {
        bsFatal("restore with a snapshot newer than current state");
    }
    // The restore's work: the journal records it replays, and one.
    auto const since = [](std::size_t now, std::size_t then) { return now - std::min(now, then); };
    work_ += 1 + since(liveScopes_.journalSize(), f.liveScopesMarkJournal)
           + since(closedParamScopes_.journalSize(), f.closedParamsMarkJournal)
           + since(headJournal_.size(), f.headJournalSize)
           + since(boundAtDeclaratorEnd_.journalSize(), f.boundAtEndMark);
    // The heads first: every change since the snapshot, newest first, back to its old value — the
    // bindings recorded since then included (their records name heads, not bindings).
    while (headJournal_.size() > f.headJournalSize) {
        HeadChange const c = headJournal_.back();
        *c.head = c.old;
        headJournal_.pop_back();
    }
    bindings_.resize(f.bindingCount);
    // erase (not resize): AmbiguousTypeNameCandidate is not default-
    // constructible (SourceSpan has factory-only construction), and
    // resize requires DefaultInsertable even when shrinking.
    candidates_.erase(
        candidates_.begin() + static_cast<std::ptrdiff_t>(f.candidateCount),
        candidates_.end());
    liveScopes_.rewindTo(TrailedStack<LiveScope>::Mark{.size    = f.liveScopesMarkSize,
                                                        .journal = f.liveScopesMarkJournal});
    closedParamScopes_.rewindTo(TrailedStack<ClosedParamScope>::Mark{
        .size = f.closedParamsMarkSize, .journal = f.closedParamsMarkJournal});
    boundAtDeclaratorEnd_.rewindTo(f.boundAtEndMark);
    nextScopeId_      = f.nextScopeId;
    version_          = f.version;   // `versionSource_` stays: a number is never given twice
    definitionParams_ = f.definitionParams;
}

} // namespace dss
