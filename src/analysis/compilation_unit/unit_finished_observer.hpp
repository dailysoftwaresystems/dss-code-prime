#pragma once

// ═══ WHO HEARS THAT A UNIT WAS BUILT — `UnitFinishedObservers` ═══════════════
// (P69 L2b2, lane `lm`)
//
// `UnitBuilder::finish()` is the one place every CompilationUnit is made: the driver,
// the editor, the FFI header reader and every test fixture build theirs through it. A
// consumer that must see EVERY unit a process builds — whoever built it, and whether or
// not the builder hands the unit on — registers here; `finish()` notifies each
// registration with the finished unit, before returning it. Nothing in this file knows
// who listens: there is no caller-specific branch, and no registration means no work
// beyond an empty list's copy.
//
// ★ WHY IT EXISTS. A unit's DRIVER diagnostics (an `#include` that resolved nowhere, a
// descriptor that would not read) live on the unit, and the driver reads them before it
// analyzes. A builder that skips that read — every test fixture did — analyzes a
// different program than the source says and stays silent about it (✔MEASURED
// 2026-11-05: 62 TESTs whose `#include <stdarg.h>` resolved nowhere, read as passing).
// The test support's listener registers here so a unit's unasked driver Error fails the
// test that built it, however the fixture was written.
//
// ⚠ THE CONTRACT A CALLBACK KEEPS: it runs on the thread that called `finish()`, while
// that unit is being returned — it reads the unit and must not keep a reference to it
// past the call. Registrations may come and go on any thread: the list is guarded, and a
// notification walks a COPY taken under the lock, so a callback that registers or
// unregisters (itself or another) never deadlocks and never invalidates the walk.

#include "core/export.hpp"

#include <cstdint>
#include <functional>

namespace dss {

class CompilationUnit;   // analysis/compilation_unit/compilation_unit.hpp

class DSS_EXPORT UnitFinishedObservers {
public:
    using Callback = std::function<void(CompilationUnit const&)>;

    // A live registration: the callback is heard until this object is destroyed (or
    // moved from and the moved-to one is). Move-only.
    class DSS_EXPORT Registration {
    public:
        Registration() noexcept = default;
        Registration(Registration&& other) noexcept;
        Registration& operator=(Registration&& other) noexcept;
        Registration(Registration const&)            = delete;
        Registration& operator=(Registration const&) = delete;
        ~Registration();

        [[nodiscard]] bool active() const noexcept { return id_ != 0; }

    private:
        friend class UnitFinishedObservers;
        explicit Registration(std::uint64_t id) noexcept : id_{id} {}
        std::uint64_t id_ = 0;
    };

    // Register `callback` for every unit finished from now on. An empty callback is a
    // caller's defect and aborts loud (it would be called and throw `bad_function_call`
    // inside some unrelated builder's `finish()`).
    [[nodiscard]] static Registration add(Callback callback);

    // Called by `UnitBuilder::finish()` with the unit it is about to return. Every
    // registration live when the call starts is called once, in registration order.
    static void notify(CompilationUnit const& unit);
};

} // namespace dss
