// P69 (lane `cs`, D-LIR-THREAD-LOCAL-IMPORT-STAMPED-READ-THROUGH-A-SLOT): a THREAD-LOCAL
// import's row must leave MIR→LIR saying its code reads NO address slot, on every shipped
// format that lowers a thread-local access.
//
// `ExternImport::readThroughSlot` is MIR→LIR's one statement of how its code reaches an
// import, and the link reads it: a row stamped `true` makes the merge mint an address slot
// for the import's definition and retarget the code's references INTO it. A thread-local's
// access is the format's TLS sequence — local-exec's thread pointer plus a link-time tpoff,
// `pe-indexed`'s `_tls_index` read, `macho-tlv`'s call through the variable's descriptor —
// and none of them loads a slot, so a stamped thread-local row sends the thread-pointer
// relocation to an address slot. ✔MEASURED at HEAD 71648598 and at this round's tree: a DSS
// `extern _Thread_local int shared;` read against gcc's and clang's objects of
// `_Thread_local int shared = 7;` was refused on both ELF execs with
// `K_RelocationKindMismatch` (the ELF writer's thread-local data-item backstop) where GNU ld
// and ld.lld link the same objects and the program exits 7. The runnable pin is
// `examples/c/thread_local_import_defined_by_a_foreign_object`.
//
// Every row below carries the OPPOSITE stale answer in, so a lowering that leaves the field
// alone, or ORs into it, cannot pass. The CONTROL is the same import without thread storage
// duration on the same format: a got-indirect DATA import IS read through its slot, which is
// what makes each thread-local arm a statement about thread storage rather than about the
// format declaring no slot at all.
//
// RED-ON-DISABLE: restore `if (e.isData)` in the constructor's got-indirect registration and
// every shipped-format arm goes red; drop `&& !e.isThreadLocal` from the indirect-slot
// registration and the weak arm goes red.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "link/object_format_schema.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

constexpr SymbolId kImport{200};

struct Lowered {
    bool                 ok = false;
    std::optional<bool>  readThroughSlot;   // the row's stamp; nullopt = no such row
    std::string          firstDiagnostic;
};

// `int f(int) { return *&import; }` — one GlobalAddr of the import and the Load of it —
// lowered with the facts `format` declares, the import carrying the stale opposite answer.
[[nodiscard]] Lowered lowerAnImportRead(TargetSchema const& target,
                                        ObjectFormatSchema const& format, CallConv cc,
                                        bool isThreadLocal, SymbolBinding binding,
                                        std::optional<TlsAccessInfo> tlsAccess) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32      = interner.primitive(TypeKind::I32);
    TypeId const i32Ptr   = interner.pointer(i32);
    TypeId const params[] = {i32};
    TypeId const fnSig    = interner.fnSig(params, i32, cc);
    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(entry);
    (void)mb.addArg(0, i32);
    MirInstId const ga         = mb.addGlobalAddr(kImport, i32Ptr);
    MirInstId const loadArgs[] = {ga};
    MirInstId const val        = mb.addInst(MirOpcode::Load, loadArgs, i32);
    mb.addReturn(val);
    Mir mir = std::move(mb).finish();

    ExternImport ei;
    ei.symbol          = kImport;
    ei.mangledName     = "shared";
    ei.isData          = true;
    ei.isThreadLocal   = isThreadLocal;
    ei.binding         = binding;
    ei.readThroughSlot = isThreadLocal;   // stale, and the opposite of the right answer
    DiagnosticReporter rep;
    auto r = lowerToLir(mir, target, interner, rep, {ei}, format.externCallDispatch(),
                        format.dataImportBinding(), tlsAccess, {}, std::nullopt,
                        format.externAddrBinding(), std::nullopt, std::nullopt,
                        format.indirectSlotBindings());
    Lowered out;
    out.ok = r.ok;
    if (!rep.all().empty()) out.firstDiagnostic = rep.all()[0].actual;
    for (auto const& row : r.externImports) {
        if (row.symbol.v == kImport.v) out.readThroughSlot = row.readThroughSlot;
    }
    return out;
}

struct Arm {
    std::string_view format;
    std::string_view target;
    CallConv         cc;
};

}  // namespace

// Every shipped format whose document declares a thread-local access model, each with the
// target it is built for. A format here that stops declaring one fails the ASSERT below by
// name rather than silently leaving the arm.
TEST(ThreadLocalImportLowering, AThreadLocalImportIsNeverStampedAsReadThroughASlot) {
    std::array<Arm, 5> const arms{{
        {"elf64-x86_64-linux-exec", "x86_64", CallConv::CcSysV},
        {"elf64-aarch64-linux-exec", "arm64", CallConv::CcAAPCS64},
        {"macho64-arm64-darwin-exec", "arm64", CallConv::CcApple},
        {"macho64-arm64-darwin-dylib", "arm64", CallConv::CcApple},
        {"pe64-x86_64-windows-exec", "x86_64", CallConv::CcMS64},
    }};
    for (Arm const& a : arms) {
        SCOPED_TRACE(std::string{a.format});
        auto const format = ObjectFormatSchema::loadShipped(a.format);
        ASSERT_TRUE(format.has_value());
        auto const target = TargetSchema::loadShipped(a.target);
        ASSERT_TRUE(target.has_value());
        ASSERT_TRUE((*format)->tlsAccess().has_value())
            << "the format declares no thread-local access model any more";
        ASSERT_EQ((*format)->dataImportBinding(), DataImportBinding::GotIndirect)
            << "the arm is about the got-indirect registration";

        Lowered const tls = lowerAnImportRead(**target, **format, a.cc, /*isThreadLocal=*/true,
                                              SymbolBinding::Global, (*format)->tlsAccess());
        ASSERT_TRUE(tls.ok) << tls.firstDiagnostic;
        ASSERT_TRUE(tls.readThroughSlot.has_value());
        EXPECT_FALSE(*tls.readThroughSlot)
            << "a thread-local import's access is the format's TLS sequence, which reads no "
               "address slot; a row stamped otherwise makes the link mint one for the "
               "definition and send the thread-pointer relocation into it";

        Lowered const control = lowerAnImportRead(**target, **format, a.cc,
                                                  /*isThreadLocal=*/false,
                                                  SymbolBinding::Global, (*format)->tlsAccess());
        ASSERT_TRUE(control.ok) << control.firstDiagnostic;
        ASSERT_TRUE(control.readThroughSlot.has_value());
        EXPECT_TRUE(*control.readThroughSlot)
            << "CONTROL: the same import without thread storage duration IS read through the "
               "format's got-indirect slot";
    }
}

// The `indirect-slot` registration, which reaches an import by its BINDING: a format may
// declare it beside a thread-local access model (today the two pe64 relocatable documents
// declare the dispatch and no access model, so this pairs the relocatable document's
// dispatch with the pe64 exec's `pe-indexed` model). A WEAK thread-local is in the
// dispatch's binding set and still reads no `.refptr` slot; a weak ordinary datum does.
TEST(ThreadLocalImportLowering, AWeakThreadLocalImportUnderAnIndirectSlotDispatchReadsNoSlot) {
    auto const relocatable = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(relocatable.has_value());
    auto const exec = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(exec.has_value());
    auto const target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    ASSERT_EQ((*relocatable)->externCallDispatch(), ExternCallDispatch::IndirectSlot);
    ASSERT_TRUE((*exec)->tlsAccess().has_value());
    bool weakIsNarrowedIn = false;
    for (SymbolBinding const b : (*relocatable)->indirectSlotBindings()) {
        if (b == SymbolBinding::Weak) weakIsNarrowedIn = true;
    }
    ASSERT_TRUE(weakIsNarrowedIn) << "the dispatch no longer routes a WEAK import through its slot";

    Lowered const tls = lowerAnImportRead(**target, **relocatable, CallConv::CcMS64,
                                          /*isThreadLocal=*/true, SymbolBinding::Weak,
                                          (*exec)->tlsAccess());
    ASSERT_TRUE(tls.ok) << tls.firstDiagnostic;
    ASSERT_TRUE(tls.readThroughSlot.has_value());
    EXPECT_FALSE(*tls.readThroughSlot)
        << "a weak thread-local's access is the `pe-indexed` sequence, not a `.refptr` read";

    Lowered const control = lowerAnImportRead(**target, **relocatable, CallConv::CcMS64,
                                              /*isThreadLocal=*/false, SymbolBinding::Weak,
                                              (*exec)->tlsAccess());
    ASSERT_TRUE(control.ok) << control.firstDiagnostic;
    ASSERT_TRUE(control.readThroughSlot.has_value());
    EXPECT_TRUE(*control.readThroughSlot)
        << "CONTROL: a weak ordinary datum under the same dispatch IS read through its slot";
}
