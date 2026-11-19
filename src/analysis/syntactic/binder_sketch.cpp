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
    // The global scope, id 0, never popped; a namespace scope. Its record first (a binding needs
    // its scope's), then the live stack.
    scopes_.push(ScopeRecord{.first = kNoBinding, .last = kNoBinding, .live = true});
    liveScopes_.push(LiveScope{.id = 0, .typeTarget = 0, .liftTarget = 0});
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
        pushBinding_(entryFor_(spelling), 0, /*isType=*/false);
    }
    // P69 L2b2 (lane `lm`, ruling 516): the language's BUILTIN functions
    // (`semantics.builtinFunctions`) are bound the same way, from the same rows the
    // analyzer binds into its builtin scope: functions the source never declares, so
    // a name the sketch does not know is one NOTHING declares — what the refusal
    // record's reader may then say (`UnknownNameRefusal`). The LIBRARY builtins
    // (`semantics.libraryBuiltins`, `__builtin_<libfn>`) are no binding: the analyzer
    // binds one at its first use, and `lookup` answers them by the same prefix and list.
    for (BuiltinFunctionMapping const& bf : sem.builtinFunctions) {
        if (bf.name.empty()) continue;
        pushBinding_(entryFor_(bf.name), 0, /*isType=*/false);
    }
    libraryBuiltins_ = &sem.libraryBuiltins;
    // The list type's spellings (`semantics.variadic.vaListTypeNames`) are TYPE names the
    // analyzer binds with no source declaration — bound here from the same list.
    for (std::string const& spelling : sem.vaListTypeNames) {
        if (spelling.empty()) continue;
        pushBinding_(entryFor_(spelling), 0, /*isType=*/true);
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

// ── P69 L2b2 (lane `lm`, the L2b1 review's W1): the chains of LIVE bindings ──
// A name's head is its newest live binding; `olderLive` / `newerLive` link the others. A binding
// joins its chain when it is recorded and leaves it when its scope closes, so a chain never holds a
// dead binding. Every link change is a write to a trailed container (the binding store, the head
// journal) made while a snapshot may be live, so a restore rewinds each to the state it marked:
// exact by the trail, whatever nesting of snapshots the parser holds.
void BinderSketch::setHead_(std::uint32_t& head, std::uint32_t to) {
    if (liveMarks_ > 0) headJournal_.push_back(HeadChange{.head = &head, .old = head});
    head = to;
}

BinderSketch::HeadEntry& BinderSketch::entryFor_(std::string name) {
    auto it = heads_.find(std::string_view{name});
    if (it == heads_.end()) it = heads_.emplace(std::move(name), kNoBinding).first;
    return *it;
}

void BinderSketch::pushBinding_(HeadEntry& name, std::uint32_t scope, bool isType) {
    if (bindings_.size() >= kNoBinding) bsFatal("more bindings than a binding index can name");
    ScopeRecord sc = scopes_[scope];
    if (!sc.live) bsFatal("a binding recorded into a scope that is not live");
    auto const index = static_cast<std::uint32_t>(bindings_.size());
    std::uint32_t const older = name.second;
    if (older != kNoBinding) {
        Binding o = bindings_[older];
        o.newerLive = index;
        bindings_.assign(older, o);
    }
    if (sc.last != kNoBinding) {
        Binding l = bindings_[sc.last];   // read after the write above: it may be the same binding
        l.nextInScope = index;
        bindings_.assign(sc.last, l);
    } else {
        sc.first = index;
    }
    sc.last = index;
    scopes_.assign(scope, sc);
    bindings_.push(Binding{.name        = &name,
                           .scope       = scope,
                           .olderLive   = older,
                           .newerLive   = kNoBinding,
                           .nextInScope = kNoBinding,
                           .isType      = isType});
    setHead_(name.second, index);
}

void BinderSketch::unlink_(std::uint32_t index) {
    Binding const b = bindings_[index];
    if (b.newerLive != kNoBinding) {
        Binding n = bindings_[b.newerLive];
        n.olderLive = b.olderLive;
        bindings_.assign(b.newerLive, n);
    } else {
        if (b.name->second != index) {
            bsFatal("a live binding with no newer one is not its name's head - a chain broke");
        }
        setHead_(b.name->second, b.olderLive);
    }
    if (b.olderLive != kNoBinding) {
        Binding o = bindings_[b.olderLive];
        o.newerLive = b.newerLive;
        bindings_.assign(b.olderLive, o);
    }
}

void BinderSketch::openScope(RuleId rule) {
    std::uint32_t const id = nextScopeId_++;
    // A scope's id is its index in the table: minted in order, rewound together by a restore.
    if (scopes_.size() != id) {
        bsFatal("openScope: a scope id that is not the scope table's next index - the table and "
                "the id counter fell out of step");
    }
    LiveScope const parent = liveScopes_.back();
    bool const dominator = dominatorScopeRules_.contains(rule.v);
    bool const composite = compositeScopeRules_.contains(rule.v);
    scopes_.push(ScopeRecord{.first = kNoBinding, .last = kNoBinding, .live = true});
    liveScopes_.push(LiveScope{.id         = id,
                               .typeTarget = dominator ? parent.typeTarget : id,
                               .liftTarget = (dominator || composite) ? parent.liftTarget : id});
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
    ScopeRecord sc = scopes_[closed.id];
    // Its bindings leave their names' chains, oldest first; its own list keeps them.
    for (std::uint32_t i = sc.first; i != kNoBinding; i = bindings_[i].nextInScope) unlink_(i);
    sc.live = false;
    scopes_.assign(closed.id, sc);
    version_ = ++versionSource_;   // the bindings it held are hidden now
    return ClosedScope{.id = closed.id};
}

bool BinderSketch::isPrototypeParamScopeRule(RuleId rule) const noexcept {
    return protoParamScopeRule_.valid() && rule == protoParamScopeRule_;
}

bool BinderSketch::isDefinitionBodyRule(RuleId rule) const noexcept {
    return definitionBodyRules_.contains(rule.v);
}

void BinderSketch::notePrototypeParamScope(ClosedScope scope, NodeId paramList) {
    closedParamScopes_.push(ClosedParamScope{.scope    = scope.id,
                                             .sequence = nextListSequence_++,
                                             .list     = paramList});
}

// ★ P69 L2b1 (lane `lm`, the L2a2 review's A1), P69 L2b2 (the L2b1 review's D1): lists nest with
// the parse, so the lists noted while a declarator or a row was open are the top of the stack —
// each numbered at or after the mark its frame read when it opened — and every entry below them was
// noted before it opened, numbered below the mark. Popping while the top is numbered at or after
// `mark` takes exactly the construct's own lists, each entry once, wherever their tokens came from.
BinderSketch::ClosedParamScope BinderSketch::consumeParamScopesFrom_(std::uint64_t mark,
                                                                     NodeId keep) {
    ClosedParamScope found{};
    while (closedParamScopes_.size() > 0) {
        ClosedParamScope const top = closedParamScopes_.back();
        if (top.sequence < mark) break;
        if (keep.valid() && top.list.v == keep.v) found = top;
        closedParamScopes_.pop();
    }
    return found;
}

void BinderSketch::keepDefinitionParamsOf(NodeId paramList, std::uint64_t declaratorMark) {
    ClosedParamScope const kept = consumeParamScopesFrom_(declaratorMark, paramList);
    // Only a declarator that names its own list replaces what is kept: a nested declarator that
    // keeps none (a parameter, a returned type's parameter) leaves the enclosing one's alone.
    if (kept.scope != 0) definitionParams_ = kept;
}

void BinderSketch::bindDefinitionParams() {
    ClosedParamScope const from = std::exchange(definitionParams_, ClosedParamScope{});
    if (from.scope == 0) return;
    // Its names, in their order, into the innermost live scope — where a definition's
    // parameters were always bound (c's topLevelDecl scope, which the body is inside). The
    // copies are appended, never moved, so a restore's rewind stays exact.
    // P69 L2b1 (the L2a2 review's A2), P69 L2b2 (the L2b1 review's P1): the list's OWN bindings,
    // read from its scope's list — the store is read through its counted accessor, so a walk of
    // more than the list (L2b1 scanned a range of the store from where the list opened) is counted.
    std::uint32_t const into = liveScopes_.back().id;
    bool any = false;
    for (std::uint32_t i = scopes_[from.scope].first; i != kNoBinding;) {
        Binding const b = bindings_[i];
        pushBinding_(*b.name, into, b.isType);
        any = true;
        i = b.nextInScope;
    }
    if (any) version_ = ++versionSource_;
}

void BinderSketch::dropDefinitionParams(std::uint64_t rowMark) noexcept {
    (void)consumeParamScopesFrom_(rowMark, NodeId{});
    if (definitionParams_.scope != 0 && definitionParams_.sequence >= rowMark) {
        definitionParams_ = ClosedParamScope{};
    }
}

void BinderSketch::record(std::string name, bool isType, bool liftToEnclosingScope) {
    if (name.empty()) return;   // anonymous/malformed decl — nothing to bind
    LiveScope const innermost = liveScopes_.back();
    // P68 round 9 (lane `cs`): a LIFTED field name (an enumeration constant) is an
    // ordinary identifier of the scope the composite specifier appears in — C
    // 6.2.1p7 — so it binds in the nearest scope that is neither a composite BODY
    // (its own enum, or a struct it is nested in) nor a declarator-dominator: the
    // block or the file, as the analyzer's republication does. The global scope
    // (slot 0) is neither, so the search always lands.
    // A composite/typedef TYPE tag (C11 6.2.1) belongs to the nearest enclosing
    // NAMESPACE scope (block or file), not an interior declarator-dominator
    // scope it may have been minted inside (a file-scope `struct P { … } v;`
    // tag is recorded as the unified structSpec's frame closes — WHILE the
    // enclosing topLevelDecl param-dominator scope is still live — and must
    // bind at file scope so it is visible to the next declaration). Float TYPE
    // bindings past dominator scopes; VALUE
    // bindings (params, locals) stay in the live scope. Mirrors the analyzer's
    // floatToNamespaceScope. (Both targets are the innermost scope's own, worked
    // out when it opened: P69 L2b2.)
    std::uint32_t const scope = liftToEnclosingScope ? innermost.liftTarget
                                : isType             ? innermost.typeTarget
                                                     : innermost.id;
    pushBinding_(entryFor_(std::move(name)), scope, isType);
    version_ = ++versionSource_;
}

void BinderSketch::recordAtDeclaratorEnd(std::string name, bool isType,
                                         bool rowOwnsInnermostScope, NodeId nameNode) {
    if (name.empty()) return;   // abstract / malformed — nothing to bind
    if (!nameNode.valid()) bsFatal("recordAtDeclaratorEnd: a name with no declaring token");
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
    LiveScope const at = liveScopes_[from];
    (void)boundAtDeclaratorEnd_.insert(nameNode.v);
    pushBinding_(entryFor_(std::move(name)), isType ? at.typeTarget : at.id, isType);
    version_ = ++versionSource_;
}

void BinderSketch::recordUnlessBoundAtItsDeclarator(std::string name, bool isType,
                                                    NodeId nameNode) {
    // The declarator-end binding noted its declaring token; the note is taken back here, once.
    if (nameNode.valid() && boundAtDeclaratorEnd_.erase(nameNode.v)) return;
    record(std::move(name), isType);
}

BinderSketch::NameKind BinderSketch::lookup(std::string_view name) const noexcept {
    // The newest LIVE binding wins — that's lexical shadowing (an inner `int T;` recorded after a
    // file-scope `typedef ... T` is found first). Only ordinary identifiers are recorded (C 6.2.3),
    // so a tag of the same spelling, however new, is never the answer: `int g(int); struct g {…};
    // (g)(2)` stays a call. A name's chain holds live bindings only, so its head is the answer.
    ++work_;   // a lookup
    auto const it = heads_.find(name);
    if (it == heads_.end() || it->second == kNoBinding) {
        // A library builtin no source binding shadows is a function (the analyzer binds
        // it at its first use); everything else unbound is Unknown.
        return libraryBuiltins_ != nullptr
                       && !libraryBuiltins_->libraryFunctionOf(name).empty()
                   ? NameKind::Value
                   : NameKind::Unknown;
    }
    Binding const b = bindings_[it->second];
    if (!scopes_[b.scope].live) {
        bsFatal("lookup: a name's newest binding belongs to a closed scope - a scope's close did "
                "not unlink it");
    }
    return b.isType ? NameKind::Type : NameKind::Value;
}

void BinderSketch::seedGlobalType(std::string name) {
    if (name.empty()) return;
    pushBinding_(entryFor_(std::move(name)), 0, /*isType=*/true);
    version_ = ++versionSource_;
}

void BinderSketch::seedGlobalValue(std::string name) {
    if (name.empty()) return;
    pushBinding_(entryFor_(std::move(name)), 0, /*isType=*/false);
    version_ = ++versionSource_;
}

void BinderSketch::recordRefusal(UnknownNameRefusal r) {
    refusals_.push_back(std::move(r));
}

BinderSketch::Snapshot BinderSketch::snapshot() {
    Snapshot s;
    ++work_;   // a snapshot: it marks every trail and copies nothing (`Snapshot::Fields`)
    auto const bindings = bindings_.mark();
    auto const scopes   = scopes_.mark();
    auto const live     = liveScopes_.mark();
    auto const closed   = closedParamScopes_.mark();
    s.f = Snapshot::Fields{
        .owner            = this,
        .depth            = liveMarks_,
        .bindings         = {.size = bindings.size, .journal = bindings.journal},
        .scopes           = {.size = scopes.size, .journal = scopes.journal},
        .liveScopes       = {.size = live.size, .journal = live.journal},
        .closedParams     = {.size = closed.size, .journal = closed.journal},
        .refusalCount     = refusals_.size(),
        .headJournalSize  = headJournal_.size(),
        .boundAtEndMark   = boundAtDeclaratorEnd_.mark(),
        .nextScopeId      = nextScopeId_,
        .version          = version_,
        .definitionParams = definitionParams_,
    };
    ++liveMarks_;
    return s;
}

BinderSketch::Snapshot::Snapshot(Snapshot&& other) noexcept : f(other.f) {
    other.f.owner = nullptr;
}

BinderSketch::Snapshot::~Snapshot() {
    if (f.owner != nullptr) f.owner->releaseMark_();
}

void BinderSketch::releaseMark_() noexcept {
    if (liveMarks_ == 0) bsFatal("a snapshot released more often than taken");
    if (--liveMarks_ == 0) {
        bindings_.discardJournal();
        scopes_.discardJournal();
        liveScopes_.discardJournal();
        closedParamScopes_.discardJournal();
        boundAtDeclaratorEnd_.discardJournal();
        headJournal_.clear();
    }
}

void BinderSketch::restore(Snapshot&& s) {
    Snapshot::Fields const& f = s.f;
    if (f.owner != this) {
        bsFatal("restore with a snapshot of another sketch, or one moved from");
    }
    // P69 L2b2: the trails rewind exactly to their marks only last in first out — the parser's
    // probes nest, and a restore of an outer snapshot while an inner one lives is a broken nesting.
    if (f.depth + 1 != liveMarks_) {
        bsFatal("restore of a snapshot that is not the innermost live one - snapshots restore last "
                "in first out");
    }
    // Refusals are append-only between snapshot and restore, so truncate-to-count is exact.
    if (f.refusalCount > refusals_.size() || f.headJournalSize > headJournal_.size()) {
        bsFatal("restore with a snapshot newer than current state");
    }
    // The restore's work: the journal records it replays, and one.
    auto const since = [](std::size_t now, std::size_t then) { return now - std::min(now, then); };
    work_ += 1 + since(bindings_.journalSize(), f.bindings.journal)
           + since(scopes_.journalSize(), f.scopes.journal)
           + since(liveScopes_.journalSize(), f.liveScopes.journal)
           + since(closedParamScopes_.journalSize(), f.closedParams.journal)
           + since(headJournal_.size(), f.headJournalSize)
           + since(boundAtDeclaratorEnd_.journalSize(), f.boundAtEndMark);
    // The heads: every change since the snapshot, newest first, back to its old value.
    while (headJournal_.size() > f.headJournalSize) {
        HeadChange const c = headJournal_.back();
        *c.head = c.old;
        headJournal_.pop_back();
    }
    bindings_.rewindTo(TrailedStack<Binding>::Mark{.size    = f.bindings.size,
                                                   .journal = f.bindings.journal});
    scopes_.rewindTo(TrailedStack<ScopeRecord>::Mark{.size    = f.scopes.size,
                                                     .journal = f.scopes.journal});
    // erase (not resize): UnknownNameRefusal is not default-
    // constructible (SourceSpan has factory-only construction), and
    // resize requires DefaultInsertable even when shrinking.
    refusals_.erase(
        refusals_.begin() + static_cast<std::ptrdiff_t>(f.refusalCount),
        refusals_.end());
    liveScopes_.rewindTo(TrailedStack<LiveScope>::Mark{.size    = f.liveScopes.size,
                                                       .journal = f.liveScopes.journal});
    closedParamScopes_.rewindTo(TrailedStack<ClosedParamScope>::Mark{
        .size = f.closedParams.size, .journal = f.closedParams.journal});
    boundAtDeclaratorEnd_.rewindTo(f.boundAtEndMark);
    nextScopeId_      = f.nextScopeId;
    version_          = f.version;   // `versionSource_` stays: a number is never given twice
    definitionParams_ = f.definitionParams;
}

} // namespace dss
