# 27 — GUI Plan

**Project:** DSS Code Prime
**Status:** Draft
**Depends on:** C23 frontend (in progress), C++ frontend / Itanium + MSVC object model, Mach-O linker, `src/link/format/macho_codesign.cpp`

---

## 1. Premises

These are fixed constraints inherited from the Code Prime design, not open questions:

| Premise | Consequence for GUI |
|---|---|
| **100% AOT**, no JIT | No runtime codegen anywhere. iOS W^X is a non-issue. |
| **No GC**, deterministic cleanup | UI tree ownership must be tree-shaped or arena-backed (§8). |
| **Minimal runtime, written in HIR** | Event loop adapters and scheduler live in the runtime subset. |
| **No high-level language emitted** | No generated Swift, Kotlin, or C++ in the output path. |
| **ABIs are configurable per target** | Bindings are generated from platform metadata, not headers. |
| **Uniform UI across all targets** | No native widget sets. See §2. |

---

## 2. Core Architecture Decision

The requirement "*all UIs built with DSS Code Prime look the same*" is not an Android-only constraint. If Material 3 is rejected on Android for visual-consistency reasons, the same logic rejects **WinUI controls on Windows**, **AppKit controls on macOS**, and **UIKit controls on iOS**. Native widget sets cannot be made to match each other.

This forces a clean two-layer split:

```
┌─────────────────────────────────────────────────────┐
│  Layer 2 — DSS UI Layer  (identical on all targets) │
│  widgets, layout, text shaping, theming, animation  │
└─────────────────────────────────────────────────────┘
                          ▲
                 GPU surface + input events
                          │
┌─────────────────────────────────────────────────────┐
│  Layer 1 — Platform Shell  (thin, per-target)       │
│  window lifecycle · input · IME · insets · loop     │
└─────────────────────────────────────────────────────┘
```

**Layer 1** uses each OS's most modern *window management* API — nothing more. It is responsible for exactly five things:

1. Create/destroy a window (or scene/surface)
2. Acquire a GPU-presentable surface
3. Deliver raw input (pointer, keyboard, touch, scroll)
4. Deliver IME/text-input events
5. Expose an event-loop integration point to the scheduler (§7)

**Layer 2** owns everything visual. It is one codebase, one renderer, one widget set.

> **Key consequence:** "modern OS window library" means *window management*, not *UI toolkit*. Compose, SwiftUI, WinUI, and Material 3 are all UI toolkits and are therefore all out of scope for Layer 1.

---

## 3. Platform Matrix

| Target | Layer 1 API | Binary format | Binding source | ABI volatility |
|---|---|---|---|---|
| Windows | **WinRT `AppWindow`** (Windows App SDK), Win32 fallback | PE | `.winmd` (ECMA-335) | Low — WinRT ABI guaranteed |
| Linux | **Wayland `xdg-shell`**, XCB fallback | ELF | protocol XML | Low — versioned wire protocol |
| macOS | **`NSWindow`** + `CAMetalLayer` | Mach-O | ObjC runtime (strings) | Low — frozen |
| iOS | **`UIWindowScene`** + `CAMetalLayer` | Mach-O | ObjC runtime (strings) | Low — frozen |
| Android | **`ComponentActivity` + `SurfaceView`** | DEX + ELF | `.class` descriptors | Low (see §6) |

Four of five targets are native-code backends against stable ABIs. Android is the only one requiring a bytecode backend.

---

## 4. Binding Generation — No Headers

No platform requires parsing C/C++ headers to reach its window layer. Every one exposes machine-readable metadata. This decouples the GUI work from C++ frontend completeness.

| Target | Metadata | Generator output |
|---|---|---|
| Windows | `.winmd` — ECMA-335 tables | COM vtable slot indices, IIDs, method signatures |
| macOS / iOS | ObjC runtime — `objc_getClass`, `sel_registerName` take **strings** | Direct `objc_msgSend` call sites |
| Linux | `wayland.xml`, `xdg-shell.xml` | Interface/opcode tables, message marshalling |
| Android | `.class` constant pool + descriptors | JNI method IDs, signature strings |

**Implication:** Layer 1 can be implemented before the C++ frontend is complete. Only libc-level system headers gate on the C23 frontend.

### 4.1 Windows specifics

- Do **not** consume `C++/WinRT`. It is among the heaviest C++20 template codebases in existence and would gate GUI work on a near-complete C++ frontend.
- Instead: parse `.winmd` directly, emit COM vtable calls from LIR. This is the approach the Rust `windows` crate takes.
- Activation path: `RoGetActivationFactory` / `RoActivateInstance` from `combase.dll`.
- `IUnknown` (3 slots) → `IInspectable` (3 more) → interface slots. Slot indices come from `.winmd` ordering.
- Windows App SDK is framework-dependent: call `MddBootstrapInitialize` before any WinRT App SDK type.

### 4.2 Apple specifics

The Objective-C runtime resolves **by string**, so zero headers are required:

```
objc_getClass("NSWindow")
sel_registerName("initWithContentRect:styleMask:backing:defer:")
objc_msgSend(...)   // cast to exact signature — mandatory on arm64
```

`objc_msgSend` must be cast to the precise call signature at every call site. On arm64 the `_stret` / `_fpret` variants no longer exist, which simplifies emission.

### 4.3 Linux specifics

Wayland is a wire protocol, not a library. Two options:

- **A:** link `libwayland-client.so.0` (C ABI, stable)
- **B:** implement the wire protocol directly over the socket — object IDs, opcodes, `SCM_RIGHTS` fd passing

Option B removes a runtime dependency entirely and is consistent with the minimal-runtime premise. The protocol is defined in XML and is stable; the generator in §4 produces the marshalling tables.

Required interfaces: `wl_compositor`, `wl_surface`, `xdg_wm_base`, `xdg_surface`, `xdg_toplevel`, `wl_seat`, `wl_keyboard`, `wl_pointer`, `wp_fractional_scale_v1`, `wp_viewporter`, `zwp_text_input_v3`.

Keymap handling requires **xkbcommon** semantics (the compositor sends an XKB keymap fd).

---

## 5. Session Detection (Linux only)

Linux is the only target requiring runtime discovery. Never trust environment variables alone — always attempt the connection.

```
if getenv("WAYLAND_DISPLAY") and wl_display_connect() succeeds:
    → Wayland backend
elif getenv("DISPLAY") and xcb_connect() reports no error:
    → XCB backend  (also covers XWayland)
elif /dev/dri/card0 opens:
    → DRM/KMS  (compositor/kiosk mode only — no windowing)
else:
    → no graphical output
```

`XDG_SESSION_TYPE` is a hint, never the decider. DRM/KMS is a different *mode of operation*, not a peer fallback: there is no window manager and the process owns scanout.

---

## 6. Android — DEX + `.so`

### 6.1 Compose is not required

Because Layer 2 renders its own widgets, **Compose provides no value** to this architecture. Dropping it removes the single highest-risk item in the entire GUI plan.

The Compose compiler↔runtime contract (`$composer` threading, `$changed` bitmasks, group keys, restart scopes, `$default` masks) is a **private agreement, not a stable ABI**. It changes between releases. Emitting it from LIR would pin Code Prime to an exact Compose runtime version and make every upgrade a backend task.

**Decision: target `ComponentActivity` + `SurfaceView` directly. No Compose, no Material 3.**

This still yields fully modern OS integration:

| Need | Provided by |
|---|---|
| Lifecycle | `ComponentActivity` (androidx.activity) |
| Window insets / edge-to-edge | `WindowCompat`, `WindowInsetsController` |
| Predictive back | `OnBackPressedDispatcher` |
| IME | `InputMethodManager` + `InputConnection` |
| Surface | `SurfaceView` → `ANativeWindow_fromSurface` |

The emitted DEX drops to a small, stable surface with **no volatile convention to track**.

### 6.2 Emitted DEX (minimum)

```
class dss/AppActivity extends androidx/activity/ComponentActivity
    implements android/view/SurfaceHolder$Callback

    <clinit>()V            → System.loadLibrary("dsscodeprime")
    <init>()V              → super()
    onCreate(Bundle)V      → super.onCreate; new SurfaceView; setContentView; addCallback
    surfaceCreated(SurfaceHolder)V        → native
    surfaceChanged(SurfaceHolder;III)V    → native
    surfaceDestroyed(SurfaceHolder)V      → native
```

Native methods carry `ACC_NATIVE` and no `code_item`.

### 6.3 DEX ↔ `.so` bridge

Use `RegisterNatives`, not name-mangling convention:

1. `System.loadLibrary` in `<clinit>` → `dlopen`
2. `JNI_OnLoad` in the `.so`
3. `env->RegisterNatives(clazz, methods, n)` with explicit `{name, signature, fnptr}`

This gives explicit binding with no mangling dependency.

### 6.4 Launch flow

1. Zygote fork → ART process (`app_process`); the executed ELF is the runtime, not our code
2. `ActivityThread.main()` starts the main Looper
3. `LoadedApk` builds a `PathClassLoader` over `classes.dex`
4. `Instrumentation.newActivity()` loads `dss.AppActivity` **by manifest name**
5. `<clinit>` → `System.loadLibrary` → `JNI_OnLoad` → `RegisterNatives`
6. `onCreate` → `SurfaceView` attached
7. `surfaceCreated` → native → `ANativeWindow_fromSurface` → Vulkan swapchain
8. Render loop driven by `ALooper` / `AChoreographer`

Steps 1–5 are unavoidable and live entirely in the managed world. There is no native entry point that bypasses them.

### 6.5 APK artifacts

| File | Format | Emitter status |
|---|---|---|
| `AndroidManifest.xml` | **binary AXML** (string pool + resource map) | to build |
| `classes.dex` | DEX (public, stable spec) | to build |
| `lib/arm64-v8a/libdsscodeprime.so` | ELF | existing backend |
| `resources.arsc` | resource table | needed if theme referenced |
| signature v2/v3 | ZIP block | to build |

DEX format is public and versioned (035→041 over ~15 years). Unlike the Compose convention, it is a safe emission target.

### 6.6 If Compose interop is ever required

Should native-widget embedding become necessary later, the mitigation stack is:

1. **Convention as data** — a versioned `compose-profile-N.json` describing method descriptors, bitmask rules, group-key scheme. Backend stays version-agnostic.
2. **Derive the profile from the artifact** — extract descriptors from `androidx.compose.runtime` `.class` files rather than hand-writing them.
3. **Differential oracle in CI** — keep `kotlinc` + Compose plugin as a *reference only* (never shipped). Compile a corpus both ways, diff normalized Composer call sequences. On upgrade, the diff identifies the exact change.
4. **Pin and bundle** — the Compose runtime ships inside the APK, so upgrades are always opt-in.
5. **Build-time hash guard** — profile records a descriptor hash; mismatch fails the build, never runtime.

This is deliberately deferred. It is a large subsystem and is not required by §6.1.

---

## 7. Event Loop Integration

The scheduler **cannot own the main thread**. Every platform has a main loop that cannot be replaced; the runtime must integrate with it.

| Target | Native loop | Integration point |
|---|---|---|
| Linux/Wayland | `wl_display_dispatch` | raw **fd** → `epoll` (simplest case) |
| Windows | message pump | `MsgWaitForMultipleObjectsEx` |
| macOS | `CFRunLoop` | `CFRunLoopSource` / `CFFileDescriptor` |
| iOS | `CFRunLoop` | same as macOS |
| Android | `Looper` | `ALooper_addFd`, `AChoreographer` for vsync |

### 7.1 Runtime contract

The HIR runtime exposes a minimal shape that each shell adapts:

- **`waker`** — an OS-signalable handle (eventfd, `PostMessage`, `CFRunLoopSourceSignal`, `ALooper_wake`)
- **`ready_queue`** — tasks eligible to run
- **`main_thread_executor`** — required, since all five toolkits demand UI calls on the main thread

Main-thread affinity must be modelled from day one; retrofitting it is expensive.

### 7.2 Coroutine model

**Stackless**, not stackful.

| | Stackful (green threads) | Stackless |
|---|---|---|
| Frames | per-task stack | compiler-allocated frame |
| Runtime weight | heavy (alloc/grow/switch) | **minimal** |
| C FFI | problematic | transparent |
| Fits AOT | acceptable | **natural** |

Stackless is the same state-machine lowering as C++20 coroutines, which the frontend already requires. The scheduler itself reduces to a queue plus an executor — small enough to write in HIR.

**Sharp edge:** suspended coroutine frames capture references outliving their creating scope. Without GC, frame-capture ownership needs an explicit answer (this is what forced `Pin` and async-specific lifetime rules in Rust). Decide this alongside the coroutine design, not after.

---

## 8. Ownership Model for the UI Tree

Scope-based cleanup handles **trees**, not **graphs**. A retained view tree with parent back-pointers, delegates, and observers is the canonical cycle case — which is why AppKit/UIKit use ARC and why SwiftUI/Compose sidestep it with immutable descriptions owned by a runtime.

Options:

| Model | Cycles | Fit with AOT + no GC |
|---|---|---|
| Refcount + explicit weak | leaks without discipline | acceptable |
| **Arena / region per frame or per window** | none possible | **strong** |
| Immutable descriptions, runtime-owned tree | none possible | strong |

**Recommendation:** arena-backed retained tree, with the arena scoped to the window. Widget nodes never individually freed; the arena is released with the window. Frame-local allocations (layout scratch, draw lists) use a per-frame arena reset each vsync.

---

## 9. Rendering

Layer 2 is one renderer. Backends:

| Target | API | Surface acquisition |
|---|---|---|
| Linux | Vulkan | `VK_KHR_wayland_surface` |
| Windows | Vulkan (D3D12 optional) | `VK_KHR_win32_surface` |
| Android | Vulkan | `ANativeWindow` → `VK_KHR_android_surface` |
| macOS | Metal | `CAMetalLayer` on `NSView` |
| iOS | Metal | `CAMetalLayer` on `UIView` |

Metal is used natively on Apple rather than Vulkan-via-MoltenVK: MoltenVK is a third-party runtime dependency, which conflicts with the minimal-runtime premise.

Text shaping, glyph rasterization, and font fallback are Layer 2 responsibilities and must be platform-independent to preserve visual identity. Platform font *enumeration* is Layer 1; shaping is not.

---

## 10. Code Signing Status

| Target | Requirement | Status |
|---|---|---|
| Linux | none | — |
| Windows | none for local; Authenticode for distribution | not started |
| Android | self-signed v2/v3 (no third party) | not started |
| macOS | ad-hoc sufficient locally (mandatory on Apple Silicon) | **done** — `macho_codesign.cpp` |
| iOS Simulator | ad-hoc | covered by the same emitter |
| **iOS device** | full CMS + provisioning profile | **not started** |

### 10.1 Gap to real signing

`macho_codesign.cpp` currently emits a SuperBlob with `count = 1` (CodeDirectory only), `flags = kCsAdhoc`, `teamOffset = 0`, `nSpecialSlots = 0`. Real signing requires:

1. **CMS blob** — `CSSLOT_SIGNATURESLOT` (0x10000), magic `0xFADE0B01`. Detached PKCS#7 over the CD hash. Needs an ASN.1/DER encoder, RSA PKCS#1 v1.5 + SHA-256, cert chain (leaf + Apple WWDR + Apple Root), and Apple's signed attributes — OIDs `1.2.840.113635.100.9.1` (cdhashes plist) and `.9.2` (DER cdhash).
2. **Clear** the `kCsAdhoc` flag.
3. **Team ID** at `teamOffset`.
4. **Special slots** — Info.plist (−1), Requirements `0xFADE0C01` (−2), CodeResources (−3), Entitlements `0xFADE7171` (−5), **DER entitlements `0xFADE7172` (−7, mandatory since iOS 15)**.

**Layout note:** special slot hashes are stored *before* `hashOffset` in reverse order. The current computation

```cpp
hashOffset = identOffset + identifier.size() + 1;
```

must become

```cpp
hashOffset = identOffset + ident.size() + 1 + teamID.size() + 1
           + nSpecialSlots * kCsHashSizeSha256;
```

with `codeDirectoryLength` adding `nSpecialSlots * 32`. `hashOffset` still points at slot 0; negative slots grow backwards.

Bundle-level additions: `embedded.mobileprovision`, `_CodeSignature/CodeResources`, binary-plist `Info.plist`.

### 10.2 Provisioning

A provisioning profile is an Apple-signed PKCS#7 plist authorizing *who* (certificates by Team ID), *what* (App ID), *where* (device UDIDs, for dev/ad-hoc), plus granted entitlements and an expiry. It is **issued by Apple**, not generated by the toolchain — the toolchain only embeds it and signs with the matching identity.

Requires Apple Developer Program (USD 99/yr). Profiles are obtainable programmatically via the App Store Connect API (JWT with a `.p8` key), so retrieval can be automated. `rcodesign` can bridge the CMS gap on non-Mac hosts until §10.1 is implemented.

---

## 11. Sequencing

Layer 1 has **no dependency on the C++ frontend** (§4) and can proceed in parallel with frontend work.

| Phase | Deliverable | Rationale |
|---|---|---|
| **1** | Wayland shell + Vulkan surface | Cheapest target; fd-based loop; protocol from XML |
| **2** | Layer 2 skeleton — arena tree, layout, draw list, text | Establishes the uniform UI contract early |
| **3** | macOS shell (`NSWindow` + Metal) | ObjC runtime is string-based; no headers needed |
| **4** | Windows shell (`.winmd` reader → `AppWindow`) | Metadata reader is the main new subsystem |
| **5** | Android shell (AXML + DEX emitter + JNI) | Bytecode backend; largest new machinery |
| **6** | iOS shell + CMS signing | Reuses phase 3 almost entirely; signing is the real work |

Phases 1–2 together prove the whole architecture. Phase 5 is the only one introducing a backend of a different kind.

---

## 12. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Coroutine frame ownership without GC | **High** | Decide with coroutine design (§7.2), not after |
| DEX + AXML emitters are new backend classes | Medium | Both formats are public and stable |
| MSVC C++ ABI is undocumented | Medium | Only blocks C++ interop, not GUI (§4) |
| iOS CMS/ASN.1 implementation cost | Medium | `rcodesign` as bridge; formats are frozen, so write-once |
| Text shaping quality across scripts | Medium | Owned by Layer 2; budget explicitly |
| Wayland extension fragmentation | Low | Negotiate via `wl_registry` at runtime; degrade gracefully |
| Compose convention drift | **Eliminated** | Compose dropped (§6.1) |

---

## 13. Resolved Decisions

Guiding principle: **maximum stability**. Where a choice trades effort for stability, take the stable side.

### 13.1 Native widget embedding — **No**

No native *widgets*. §6.6 stays deferred on every platform.

However, a narrow class of **OS-owned services** cannot be drawn by Layer 2 and must be used natively. These are not widgets — they are system chrome, and users expect them to look native:

| Service | Why unavoidable |
|---|---|
| IME candidate window | Drawn by the input method, not the app |
| File picker | Sandbox/permission model on iOS & Android grants access *through* the picker |
| Share sheet, print, permission prompts | OS-owned, no API to replicate |
| WebView | Only if ever required |
| Camera preview / DRM video | Hardware overlay planes |

Layer 1 owns these. They do not compromise visual identity because they are not part of the application's UI surface.

### 13.2 Accessibility — **Mandatory, exported semantics tree**

Not optional: with Layer 2 rendering its own widgets, assistive technology sees **nothing** unless a tree is exported. This is also a regulatory requirement in several markets.

Design: Layer 2 maintains a **semantics tree** parallel to the widget tree — role, label, value, state, bounds, actions, focus order. Layer 1 provides per-platform exporters. See §15.

### 13.3 Multi-window — **Model N from day one, ship 1**

The architecture assumes an array of windows; the initial implementation opens one. See §14.6 for what multi-window means and why retrofitting is expensive.

### 13.4 Scaling — **DSS-normalized**

Layer 2 lays out in **logical units (DIPs)** with a single `float` scale factor per window. Layer 1 reports the factor and its changes; platform layout semantics are never inherited. Snapping to physical pixels happens only at raster time.

| Target | Source of scale | Notes |
|---|---|---|
| Windows | Per-Monitor V2, `WM_DPICHANGED` | must declare awareness in manifest |
| macOS | `backingScaleFactor` | typically integral (1×/2×) |
| iOS | `UIScreen.nativeScale` | fixed per device |
| Wayland | `wp_fractional_scale_v1` | 1/120ths; pair with `wp_viewporter` |
| Android | `densityDpi` | arbitrary float |

Scale changes at runtime (monitor switch, user setting) must be handled — treat as a window resize plus a raster-cache invalidation.

---

## 14. Layer 2 Scope

Choosing a uniform UI means Layer 2 must own everything a native toolkit would have provided. This is the true cost of the decision and is scoped here explicitly.

### 14.1 Text

The largest single subsystem, and the most commonly underestimated:

- **Shaping** — complex scripts (Arabic joining, Indic reordering, CJK), OpenType feature application
- **Bidirectional layout** — Unicode Bidirectional Algorithm (UAX #9)
- **Line breaking** — UAX #14; word segmentation UAX #29
- **Grapheme clustering** — cursor movement, selection, deletion must operate on graphemes, not code points
- **Font fallback** — per-script chains; platform font *enumeration* is Layer 1
- **Rasterization** — hinting, subpixel AA/gamma, colour emoji (COLR/CBDT/sbix)
- **Unicode tables** — must be embedded; this is an ICU-scale data dependency and needs an explicit size budget

### 14.2 Layout & composition

- Constraint/flex layout model, intrinsic sizing, baseline alignment
- Damage tracking and partial repaint
- Layer tree: clipping, opacity, blend modes, transforms
- Hit testing; z-order and overlay/popup layers

### 14.3 Interaction

- Focus management and keyboard navigation order
- Text editing: selection, caret, undo stack, composition state during IME
- Gesture recognition: tap, drag, long-press, pinch, fling
- Scroll physics (see §14.7)
- Context menus, tooltips, drag-and-drop payload model

### 14.4 Presentation

- Theming/design tokens; light/dark
- Animation and frame pacing, vsync-driven
- RTL mirroring of layout and iconography
- Colour management — sRGB vs Display P3 vs HDR. Apple platforms are wide-gamut by default; without an explicit pipeline the *same* colours will not match across targets, defeating the uniformity goal

### 14.5 Platform-reported state Layer 2 must respect

Supplied by Layer 1, consumed by Layer 2:

- Safe-area insets: notch/cutout, system bars, IME height
- System preferences: reduced motion, high contrast, system font scale, dark mode
- Cursor shape requests (set by Layer 1 on Layer 2's behalf)
- Clipboard and drag-and-drop with format negotiation

### 14.6 Multi-window — definition and rationale

**Multi-window** means the application presenting more than one top-level OS window at once: two documents side by side, a detached inspector or palette, a separate preferences window, a tear-off panel. It is distinct from in-app panels, which Layer 2 draws inside one window.

Platform support tiers:

| Target | Support | Form |
|---|---|---|
| Windows / Linux / macOS | Full | normal desktop idiom |
| iOS | Partial | scene-based (iPad multi-window, Stage Manager) |
| Android | Effectively none | split-screen is separate activities; freeform/desktop mode is rare |

**Why model it now:** N-window support touches the renderer (one swapchain per window), event routing, arena scoping (§8 already scopes arenas per window), the main-thread executor, and the entire shell API. Declaring the shell API over `Window[]` from the start is nearly free; retrofitting it later is not.

### 14.7 What must stay platform-specific

Uniform *visuals* must not become uniform *behaviour*. Copying one platform's interaction conventions everywhere makes the application feel broken on the others. The following are deliberately per-platform:

| Behaviour | Divergence |
|---|---|
| Modifier keys | `Cmd` on Apple, `Ctrl` elsewhere |
| Editing keybindings | macOS emacs-style bindings; Home/End semantics |
| Menu placement | macOS global menu bar vs in-window |
| Scroll direction & physics | iOS rubber-band, Android overscroll, desktop wheel |
| Right-click / secondary action | trackpad and touch conventions |
| Back navigation | Android predictive back, iOS edge swipe |
| File dialogs | always native (§13.1) |

Layer 2 exposes these as a **platform behaviour profile** consumed by input handling — not as visual differences.

---

## 15. Accessibility Architecture

Layer 2 emits a semantics tree; Layer 1 exports it. Node model: role, label, description, value, state flags, bounds (in window coordinates), supported actions, and children.

| Target | Platform API | Notes |
|---|---|---|
| Windows | **UI Automation** (`IRawElementProviderSimple` / `Fragment`) | classic COM in `uiautomationcore.dll`, *not* WinRT — separate binding path from §4.1 |
| macOS | **NSAccessibility** | informal protocol implemented on the hosting `NSView` |
| iOS | **UIAccessibility** | `UIAccessibilityElement` array on the hosting `UIView` |
| Linux | **AT-SPI2 over D-Bus** | requires a D-Bus client in the runtime — a real addition to the minimal-runtime budget |
| Android | **`AccessibilityNodeProvider`** | virtual view hierarchy; adds methods to the emitted DEX (§6.2) |

Two consequences worth flagging early:

- **Linux needs D-Bus.** AT-SPI2 has no alternative transport. Either implement a minimal D-Bus client in HIR or accept no screen-reader support on Linux.
- **Android's DEX grows.** The virtual hierarchy is expressed through Java-side callbacks, so §6.2's class gains an `AccessibilityNodeProvider` implementation delegating to native.

Also required: focus order, live-region announcements, and honouring system font scale and high-contrast settings (§14.5).

---

## 16. Configuration Descriptors

Object formats are already data (`src/dss-config/object-formats/*.json`) consumed by the format-blind linker engine. GUI and packaging extend the same discipline rather than introducing new mechanisms.

### 16.1 Layout

`dss-config/` already hosts independent axes side by side — object formats and the language file (`sources/c-subset.lang.json`) each own half of the entry-point intersection. Packaging is a third axis and sits at the same level:

```
src/dss-config/
  object-formats/    pe64-x86_64-windows-guiexec_format.json
  package-formats/   apk-android_pack.json
                     app-ios_pack.json
  sources/           c-subset.lang.json
```

### 16.2 GUI as a separate object format

`.obj` and `exec` are already **separate files**, not a mode flag on one file. GUI exec follows the same precedent: a third file, with no override or precedence semantics to invent.

Delta against the CLI exec descriptor — everything else is unchanged:

```json
{
  "format": { "name": "pe64-x86_64-windows-guiexec", "kind": "pe" },

  "artifactProfiles": ["gui"],

  "optionalHeader": { "subsystem": 2 },

  "windowVehicle": "win32",

  "runtimeLibraries": [
    { "role": "windowVehicle", "image": "user32.dll" },
    { "role": "gpuVehicle",    "image": "vulkan-1.dll" }
  ]
}
```

`entryVerbs` is unchanged: under subsystem 2 the UCRT still populates `__argc`/`__argv`, so the declared verbs remain realizable. The real difference is that no console is attached — standard output goes nowhere.

### 16.3 `windowVehicle` — enumerator plus engine arm

Per the existing rule that enumerators arrive **with their engine arms**, never as a respelling of an existing one:

| Item | Owner |
|---|---|
| `subsystem`, extra imports, entry symbol | **data** — object format |
| `windowVehicle` (`win32` / `wayland` / `xcb` / `appkit` / `uikit` / `android-surface`) | **enumerator + engine arm** |
| Callback contract, surface state machine, loop integration | **engine only** — never data |

This mirrors `entryVerbs`: the object format declares *which window vehicles it can realize*; the Layer 1 backend owns the shell itself. The accepted set is an intersection, and the format file owns only half of it.

### 16.4 `package-formats` — why it cannot live in the object format

A package descriptor's `inputs` is a **list**. An APK references **two different object formats** (a DEX plus one ELF per ABI); an `.app` bundles a Mach-O with `Info.plist`, `CodeResources`, and a provisioning profile. Neither input can own the container, so the container is a sibling schema.

```json
{
  "dssPackageFormatVersion": 1,
  "pack": { "name": "apk-android", "kind": "zip" },

  "inputs": [
    { "role": "bytecode", "format": "dex-android",
      "path": "classes.dex" },
    { "role": "native",   "format": "elf-aarch64-android",
      "path": "lib/{abi}/lib{name}.so", "perAbi": true }
  ],

  "metadata": [
    { "kind": "axml", "path": "AndroidManifest.xml" }
  ],

  "signing": { "scheme": "apk-v2", "vehicle": "zip-block" }
}
```

```json
{
  "dssPackageFormatVersion": 1,
  "pack": { "name": "app-ios", "kind": "directory" },

  "inputs": [
    { "role": "executable", "format": "macho-arm64-ios", "path": "{name}" }
  ],

  "metadata": [
    { "kind": "bplist",       "path": "Info.plist" },
    { "kind": "provisioning", "path": "embedded.mobileprovision" }
  ],

  "signing": {
    "scheme": "macho-cms",
    "sealedResources": "_CodeSignature/CodeResources"
  }
}
```

Signing belongs here, not in the object format: it is a property of the bundle. `macho_codesign.cpp` still emits the signature; the package descriptor is what declares that it needs special slots, a provisioning profile, and sealed resources (§10.1).

**Conventions:**

- `inputs[].format` resolves against the object format's `format.name`, never a file path — name references survive file moves.
- `dssPackageFormatVersion` is present from the first file; retrofitting a version field later is expensive.

### 16.5 CLI selection

Compilation selects **object formats and packages independently**. Both are repeatable.

```
dss build \
  --object-format pe64-x86_64-windows-guiexec \
  --object-format elf-aarch64-android \
  --object-format dex-android \
  --package apk-android
```

Resolution rules:

1. `--package` computes the **closure** of its `inputs[].format`, so the object formats it needs are implied. Listing them explicitly is allowed and redundant.

   ```
   dss build --package apk-android
   # resolves: dex-android + elf-aarch64-android (× declared ABIs)
   ```

2. **No package selected → bare artifacts.** Windows and Linux ship a plain `.exe`/ELF with no container. This is the phase 1–3 path.
3. A selected package whose declared inputs cannot be satisfied is a **build error**, not a silent partial emit.
4. `dss formats list` / `dss packages list` enumerate what the descriptors provide.

`package-formats/` only enters the critical path at phases 5–6 (Android, iOS). It can exist with no entries — or with trivial ones — until then.

---

## 17. Remaining Open Questions

1. Unicode/ICU data size budget for §14.1 — embedded tables materially affect binary size on mobile.
2. Colour pipeline target: normalize everything to sRGB (simplest, uniform, gives up wide gamut) or manage P3/HDR per target?
3. Linux screen-reader support: implement a D-Bus client, or defer AT-SPI2?
4. Font source: use platform fonts (differs per OS, breaks pixel identity) or ship an embedded font set (uniform, larger binaries, licensing)?
