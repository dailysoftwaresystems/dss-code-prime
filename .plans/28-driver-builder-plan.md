# Driver & Boot Builder — Reserved Sub-Plan (28)

> **Reserved scope.** Owns two adjacent contracts that share one substrate:
>
> 1. **`IOSDriver`** — a family of device-driver interfaces (`INetworkDriver`, `IStorageDriver`, `IGraphicsDriver`, `IInputDriver`, …) implemented **once** in [DSS Axis](./24-dss-axis-language-reserved-plan%20-%20tbd.md) and **built per (target CPU × OS × execution mode)**, so one driver source produces a Windows `.sys`, a Linux `.ko`, a macOS `.dext`, and — the destination this plan exists for — a **DSS OS** driver.
> 2. **`IBoot`** — a family of *bootable-artifact* interfaces (UEFI as the shipped default; custom vehicles authorable by anyone), so DSS Axis can produce images that run **before any OS**, on PCs and on specific hardware. This is the on-ramp to [`ZZ-final-goal`](./ZZ-final-goal.md) §7 ("OS in DSS Axis").
>
> **The stated purpose, in the author's words:** *to really make it easier to make drivers for the DSS OS in the future.* Windows / Linux / macOS are not the destination — they are the **falsification set**. An interface with one implementation proves nothing; three hostile, mutually incompatible real driver models are what prove the abstraction is an abstraction. The DSS OS then arrives as a fourth backend that *fits the contract*, rather than as the contract's only definition.
>
> **Depends on [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) (DSS Axis)** for the driver half — and imposes a lock back on it (§9.1). The **boot half is NOT Axis-dependent** and is reachable far earlier (§12.1).

---

## 0. Status (snapshot)

| | |
|---|---|
| Status | 🔒 **reserved**, with a **split trigger** (§1). The driver half opens behind DSS Axis + a freestanding language profile. The boot half opens behind three named engine gaps and needs no new language. |
| Predecessors | ⏳ [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) DSS Axis *(driver half only)* · ⏳ [`plan-21`](./21-runtime-reserved-plan%20-%20tbd.md) runtime *(the subtractable-runtime lock, §9.1)* · ⏳ [`plan-23`](./23-full-c-plan%20-%20tbd.md) full C *(kernel headers are C, and hostile C)* · ✅ [`plan-14`](./14-linker-plan%20-%20tbd.md) linker — PE/ELF/Mach-O image emission incl. PE `.reloc` · ✅ [`plan-11`](./11-ffi-plan%20-%20tbd.md)/[`plan-25`](./25-posix-per-target-structs%20-%20tbd.md) per-target descriptors + struct `variants` · ⏳ [`plan-16`](./16-codesign-publish-plan%20-%20tbd.md) codesign *(three new signing authorities, §6.5)* |
| Successors | The **DSS OS** ([`ZZ-final-goal`](./ZZ-final-goal.md) §7) — which consumes both contracts: `IBoot` to start, `IOSDriver` to talk to hardware. |
| Scope | **The two contracts + the artifact vocabulary they need.** Interface shape (up-call + down-call), the execution-mode and bus axes, the new artifact profiles / object formats / entry verbs / runtime-library roles, the payload binding (§5.3), the boot security model (§5.5), and the VM verification leg (§8). **Out of scope:** the drivers themselves, the DSS OS, and the Layer-2-equivalent device logic for any specific device class. |
| Relationship to [`plan-27`](./27-gui-plan.md) | **Structurally identical, deliberately.** Plan 27 split GUI into *thin per-OS shell* + *one portable codebase*, and made the shell selector a config **enumerator that arrives with its engine arm**. This plan applies the same split to drivers and to boot. Where 27 and 28 disagree, 27 is the precedent and this plan owes an explanation. |
| Provenance | Claims are tagged **MEASURED** (verified in this tree, 2026-08-12), **DOCUMENTED** (public vendor spec, not re-verified here), or **INFERRED**. Per [[feedback_verify_before_asserting_2026_07_26]], every DOCUMENTED claim is a **labelled suspect** until a probe confirms it; none may be built on without that probe. |

---

## 0.1 Naming — "driver" is already taken in this repo

**MEASURED.** In DSS Code Prime, *driver* already means the **compiler driver** — the CLI, `Program::compileFiles` / `compileProject`, `publish_driver.cpp` ([`plan-16`](./16-codesign-publish-plan%20-%20tbd.md) §2.9) — and `D_*` is the **reserved diagnostic-code namespace for it** ([`plan-15`](./15-debug-info-plan%20-%20tbd.md) §: *"`D_*` is reserved for the driver"*).

This plan therefore uses:

| Concept | Spelling here | Never |
|---|---|---|
| A device driver | **device driver**, `IOSDriver`, `driverVehicle` | bare "driver" in prose where the CLI could be meant |
| The compiler CLI | **the compiler driver** | — |
| Anchors, this plan | `D-DRV-*` (devices), `D-BOOT-*` (boot) | `D-DRIVER-*` (reads as the CLI) |
| Diagnostics, this plan | **open question** §10 Q7 — `D_*` is taken | — |

---

## 1. Trigger

**This plan has two independent triggers. Neither half waits on the other.**

### 1.1 Boot half (`IBoot`) — opens on three engine facts, not on a language

Opens when **all three** hold:

1. **A non-hosted exit mechanism exists.** ★ This is the real blocker, and it is sharper than "no libc". **MEASURED 2026-08-12**: `ExitMechanism` (`src/core/types/target_schema.hpp:1972`) is a closed enum of exactly **`Syscall | ByNameImport`** — and `object_format_schema.cpp:466` records the rule *"an exec-flavored format **MUST** declare `processExit` … DSS **ALWAYS** synthesises an entry trampoline for an exec-flavored format — **that is DSS POLICY, not a platform fact** — so a format that declares no exit mechanism gives the trampoline emitter nothing to call."* A UEFI application *returns `EFI_STATUS` to firmware*; a flat reset-vector image *never exits at all*. Neither is a syscall and neither is an import. So the boot half needs a **new `ExitMechanism` value with its engine arm** (e.g. `return-to-caller`) and a trampoline policy that a non-hosted format can decline. Note the comment names its own rule as *policy* — which is exactly the kind of self-aware constraint that can be extended rather than fought. → `D-BOOT-NONHOSTED-SPINE`.
2. **A format can declare no C library at all.** A UEFI image imports **nothing** (**DOCUMENTED**: every service arrives through the `EFI_SYSTEM_TABLE` pointer passed to the entry), so an **empty** `runtimeLibraries` table must be a legal answer. **MEASURED**: the two directions are enforced in *different tiers* — `validate()` (`object_format_schema.cpp:594-644`) checks that every role a spine block **claims** resolves to a row, while the inverse ("a row no block names is inert config") is enforced in the **JSON loader**, deliberately, because a block rejected for an unrelated defect would otherwise cascade into a spurious `/runtimeLibraries` error. Both directions are vacuously satisfied by an empty table with no claiming blocks — so the empty case looks *expressible*, but that is **INFERRED from reading the checks, not measured by building such a format**, and must be probed before it is relied on. → `D-BOOT-NONHOSTED-SPINE`.
3. **The `boot` artifact profile and its structural non-executability gate exist** (§5.5, §6.1). → `D-BOOT-ARTIFACT-NOT-HOSTED-EXECUTABLE`.
4. **The payload binding is configurable** (§5.3) — a boot image with no way to name the kernel it hands off to is a demo, not a deliverable. → `D-BOOT-PAYLOAD-BINDING`.

**What is NOT a blocker, and this is the headline:** ★ **MEASURED 2026-08-12 — the PE writer already emits `.reloc` base relocations.** `pe64-x86_64-windows-exec.format.json`'s `$comment` on `relocations[]` records that the image is ASLR-aware (`dllCharacteristics` carries `DYNAMIC_BASE = 0x0040`) and *"the walker DOES emit a `.reloc` base-relocation section carrying an `IMAGE_REL_BASED_DIR64` entry per 8-byte absolute fixup."* Firmware-relocatable PE images are the single hardest PE-side prerequisite for UEFI, and **it is already in the tree**. Combined with the fact that **UEFI on x86_64 uses the Microsoft x64 calling convention** (**DOCUMENTED**) — which DSS already emits — the distance from today's engine to a running UEFI image is *unusually short*. See §12.1.

### 1.2 Driver half (`IOSDriver`) — opens behind DSS Axis

Opens when **all** hold:

1. **[`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) has opened** and DSS Axis can express an interface family with per-target implementations.
2. **A freestanding language profile exists** (§9.1) — no unwinder, no scheduler, no libc, no implicit allocation. ★ **LARGELY RESOLVED 2026-08-12, and by a cleaner mechanism than this plan asked for.** *This clause formerly read: "no GC … Plan 24 is async-first with an async GC as its moat; none of that can exist inside a kernel driver."* [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §2.3 **removed garbage collection outright** in favour of proven-static reclamation ([`plan-09.5`](./09.5-dss-hir-plan.md) §4), and split the language config into two **independent** blocks: `memory` (a compile-time pass) and `runtime.enable` (the linked services). ⇒ **`memory` on + `runtime.enable` off IS the freestanding profile** — a driver keeps **full ownership analysis at zero runtime cost** and links no runtime at all. What this plan asked to be *kept reachable* is now an existing config switch, so the remaining obligation is only to **pin that the disabled path stays complete** ([`plan-09.5`](./09.5-dss-hir-plan.md) `D-DSSHIR-DISABLED-PATH-COMPLETE`). → `D-DRV-FREESTANDING-LANGUAGE-PROFILE`.
3. **Full C ([`plan-23`](./23-full-c-plan%20-%20tbd.md)) has shipped.** Every one of the three foreign driver models is reached through C headers, and they are the most hostile C in existence (kernel headers use every GNU extension, every attribute, and — on Linux — macros that generate the module metadata itself).
4. **At least one bus is modelled end-to-end** (§3.3), so the class axis and the transport axis are proven separable rather than asserted separable.

Until then, **design is explicitly deferred** and this file's job is §9 — the locks that keep both halves *reachable*.

---

## 2. Thesis — the two-layer split, twice

[`plan-27`](./27-gui-plan.md) §2 established the shape: a **thin per-platform shell** under **one portable codebase**, with the shell selected by a config enumerator that arrives *with* its engine arm. The same split is correct here, and for the same reason: the five OS driver models cannot be made to resemble each other, so pretending they can is how the abstraction dies.

```
┌────────────────────────────────────────────────────────────────┐
│  Layer D2 — the driver itself  (identical on every target)     │
│  device logic, protocol state machines, register programming   │
│  implements  IOSDriver / INetworkDriver / IStorageDriver / …   │
└────────────────────────────────────────────────────────────────┘
        ▲ up-calls (§3.1)              │ down-calls (§3.2)
        │  the OS calls INTO us        ▼  we call INTO the OS
┌────────────────────────────────────────────────────────────────┐
│  Layer D1 — the driver vehicle  (thin, per OS × mode)          │
│  WDM/KMDF · UMDF · Linux LKM · VFIO/UIO · DriverKit · DSS OS   │
│  registration, dispatch tables, lifecycle, IDriverHost impl    │
└────────────────────────────────────────────────────────────────┘
```

**The load-bearing difference from plan 27, stated once:** a GUI shell is *almost all up-call* — the OS hands you a window and events, and you draw. A device driver is **half down-call**, and the down-half is the hard half. What the OS calls into you (`probe`, `remove`, `read`, `write`) is a shallow, easily-abstracted surface. What you call into the OS — map this BAR, pin these pages, get me a DMA-coherent buffer under this IOMMU domain, register this interrupt, am I allowed to sleep right now — is deep, per-OS, and is where every naïve driver-portability layer in history has broken. **An interface that only faces up is unimplementable.** Hence §3.2 is a first-class deliverable, not an appendix.

The boot half takes the same shape with a different waist:

```
┌────────────────────────────────────────────────────────────────┐
│  Layer B2 — the payload  (a bootloader, a kernel, an OS)       │
└────────────────────────────────────────────────────────────────┘
        ▲ handoff (§5.1)               │ firmware services
┌────────────────────────────────────────────────────────────────┐
│  Layer B1 — the boot vehicle                                   │
│  UEFI (default) · multiboot2 · U-Boot · flat reset-vector · …  │
└────────────────────────────────────────────────────────────────┘
```

---

## 3. The contracts

### 3.1 `IOSDriver` — the up-call family

`IOSDriver` is the **base lifecycle** every device driver has, regardless of class. The class interfaces refine it; they never replace it.

```
IOSDriver                       lifecycle every vehicle can express
  probe(DeviceHandle) -> Result     device matched; claim or decline
  start()             -> Result     bring up; resources are valid after this
  stop()              -> Result     quiesce; no new I/O accepted
  remove()            -> Result     release everything acquired in start
  suspend(PowerState) -> Result     power transitions (§3.2 owns the rest)
  resume(PowerState)  -> Result
```

Class interfaces (initial set; the vocabulary is **registered and extensible**, exactly like `artifactProfile` — §6.1):

| Interface | Shape of the contract | Notes |
|---|---|---|
| `INetworkDriver` | tx/rx queues, link state, MAC/MTU, offload capability set | maps to `net_device_ops` / NDIS miniport / `IOUserNetworkEthernet` |
| `IStorageDriver` | block read/write/flush/trim, geometry, queue depth | `blk_mq` / StorPort / `IOUserBlockStorageDevice` |
| `IGraphicsDriver` | mode set, surface alloc, present, fence/sync | ⚠ the least portable class — see below |
| `IInputDriver` | HID report in/out, descriptor | `evdev` / HIDClass / `IOUserHIDDevice` |
| `IAudioDriver` | ring buffer, format negotiation, clock | ALSA / PortCls / `IOUserAudioDevice` |
| `ISerialDriver` | byte stream, line settings, flow control | tty / serial.sys / `IOUserSerial` |
| `IFilesystemDriver` | namespace ops | ⚠ the only class with a *good* user-mode story everywhere (FUSE / ProjFS / FSKit) |
| `ISensorDriver` | sampled reads, thresholds, power | IIO / Sensors API |

> **Honest note on `IGraphicsDriver`.** A modern GPU driver is a compiler, a memory manager, a scheduler, and a display engine — and each OS owns a *different* half of it (WDDM's kernel/user split, DRM/KMS + a Mesa userspace, IOKit + Metal). "One `IGraphicsDriver` implemented once" is **not a claim this plan makes** for accelerated 3D. What it claims is achievable: **mode-setting and framebuffer presentation** (the display half). Accelerated 3D is named as out of scope and anchored, not quietly folded in. → `D-DRV-GRAPHICS-CLASS-SCOPE-LIMIT`.

### 3.2 `IDriverHost` — the down-call contract (the hard half)

What the driver calls **into** the vehicle. This is the portability problem.

| Group | Operations | Why it is hard |
|---|---|---|
| **MMIO** | map/unmap BAR, read/write 8/16/32/64, ordered vs relaxed | ordering rules and required barriers differ per **ISA**, not just per OS |
| **DMA** | alloc coherent, map/unmap streaming, sync-for-cpu / sync-for-device, scatter-gather | IOMMU domains, bounce buffers, and cache-coherency are modelled differently everywhere |
| **Interrupts** | register/unregister, line vs MSI vs MSI-X, threaded vs top-half, affinity | the split between "hard IRQ" and "deferred work" is a *different* concept per OS (DPC / tasklet+workqueue / IOInterruptDispatchSource) |
| **Execution context** | `may_sleep()`, `in_atomic()`, spinlock/mutex acquire, per-CPU state | ★ the single most dangerous portability trap — see below |
| **Memory** | kernel alloc (sleeping vs atomic), page pinning, virt↔phys, cacheability | |
| **Time** | monotonic clock, delay (busy vs sleeping), timers | |
| **Power** | D-state / runtime-PM transitions, wake sources | |
| **Config space** | PCI/USB descriptor read/write, capability walk | overlaps §3.3 |
| **Log / trace** | leveled log that survives early boot and IRQ context | |

★ **The execution-context rule must be in the type system, not in the documentation.** Calling a sleeping allocator from an interrupt handler is a kernel panic on Linux, an IRQL bugcheck on Windows, and a hang on bare metal — and it is a *silent* mistake at the source level. Every OS encodes this rule informally (Linux `GFP_ATOMIC` vs `GFP_KERNEL`; Windows `IRQL <= DISPATCH_LEVEL`). **DSS Axis has an effect system already committed** ([`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §3.3: *pure / IO / async / throws / allocates*). Extending it with an **`atomic-context` / `may-sleep` effect** turns the whole class of bug into a **compile error**, and does it *generically* — the effect is DSS Axis's, not any one OS's. **This is the strongest single argument that plan 28 belongs to DSS Axis rather than to a C library**, and it is the plan's most valuable idea. → `D-DRV-EXECUTION-CONTEXT-EFFECT`.

### 3.3 The bus / transport axis — orthogonal to the class axis

A network driver over PCIe and a network driver over USB are **one device implementation against two transports**, not two drivers. Modelling the bus as a separate axis is what stops the class matrix from multiplying.

| Axis | Values (initial) |
|---|---|
| **Device class** | network · storage · graphics · input · audio · serial · filesystem · sensor |
| **Bus / transport** | PCIe · USB · I²C · SPI · platform/device-tree · virtio · MMIO-direct |
| **Execution mode** | kernel · user (§4) |
| **Target** | (CPU × OS) — the existing `arch:format` axis |

A concrete build is a point in **four** spaces. The vehicle (D1) is selected by (OS, mode); the bus adapter is selected by transport; the class interface and the device logic are selected by neither. → `D-DRV-BUS-TRANSPORT-AXIS`.

> **`virtio` earns its row.** It is the one bus whose device model is identical on every OS *and* is trivially available inside QEMU — which makes it the **natural first bus** for §8's VM leg, and the one that lets the driver half be proven without a single piece of physical hardware.

---

## 4. Execution modes — kernel and user, co-equal

**Decision: co-equal from day one.** Each (OS, class) cell declares which modes it can realize; neither mode is "the real one". This is more scope than a user-mode-first plan, and it is the correct answer for one reason: **the platforms have already diverged, and picking either mode as the floor makes a whole platform unrepresentable.**

### 4.1 The reality matrix

**DOCUMENTED — every cell is a labelled suspect pending a probe (§1 provenance).**

| OS | Kernel mode | User mode | The catch |
|---|---|---|---|
| **Windows** | `.sys` — PE, `IMAGE_SUBSYSTEM_NATIVE (1)`, entry `DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING) -> NTSTATUS`, imports `ntoskrnl.exe`/`hal.dll` (+ `wdfldr.sys` for KMDF) | **UMDF** — a DLL hosted in `WUDFHost.exe` | both need a signed driver package (INF + catalog) to install; kernel mode additionally needs attestation/WHQL signing (§6.5) |
| **Linux** | `.ko` — ELF `ET_REL` + `.modinfo` (vermagic, license, depends) + `.gnu.linkonce.this_module` | **VFIO** (IOMMU-backed, the DPDK/SPDK model) · **UIO** · **FUSE** · libusb | ★ **no stable in-kernel ABI** — §4.3 |
| **Android** | vendor modules against the **GKI stable KMI** | HALs (AIDL) | ★ the *one* place Linux offers a stable module interface — a genuinely favourable cell |
| **macOS** | kexts — **deprecated**; blocked on Apple Silicon without Reduced Security | **DriverKit `.dext`** — Mach-O in a bundle, user space | family entitlements are **granted by Apple on application** (§6.5) |
| **iOS / iPadOS** | none | severely restricted DriverKit subset | treat as **out of scope pending evidence**; do not claim a cell we cannot probe |
| **DSS OS** | *ours to define* | *ours to define* | **this is the point of the plan** — see below |

★ **The most valuable row is the last one.** Because the DSS OS's driver model does not exist yet, it can be **defined as whatever makes `IOSDriver` + `IDriverHost` a natural fit** — a native, zero-shim vehicle. The three foreign models are what *discipline* that definition: a DSS OS driver model designed in isolation would be a model that only DSS OS drivers can satisfy, which is the same overfitting failure [`ZZ-final-goal`](./ZZ-final-goal.md) §8 risk #6 names for the language itself. → `D-DRV-DSSOS-NATIVE-VEHICLE`.

### 4.2 What co-equal costs

Two full vehicle families per OS, two artifact formats, two signing paths, two VM legs. The plan accepts this and states it plainly rather than discovering it later. The mitigation is that **the mode is a property of the vehicle (D1), not of the driver (D2)** — so the cost is paid once per (OS, mode) vehicle and never per driver.

### 4.3 The Linux no-stable-ABI problem — stated, not waved at

**DOCUMENTED.** Linux deliberately has **no stable in-kernel API or ABI**. A `.ko` carries a `vermagic` string that must match the running kernel exactly, and with `CONFIG_MODVERSIONS` a CRC per imported symbol. Consequences DSS cannot engineer away:

1. **A Linux kernel module is built against one specific kernel build**, not against "Linux". The build inputs include that kernel's headers and `Module.symvers`. This is why DKMS exists.
2. **`EXPORT_SYMBOL_GPL` gates a large part of the kernel** behind a `MODULE_LICENSE` declaration. This is a **licensing** consequence of a technical choice and it lands on the *user's* driver source, not on DSS. → §10 Q4; **a human decision, not an engineering one.**
3. **Secure Boot** requires modules signed by a key in the kernel keyring (MOK-enrolled).

Three honest options, to be settled at trigger time (§10 Q3): per-kernel builds (DKMS-shaped), restrict Linux to the **Android GKI stable KMI**, or restrict Linux to **user mode (VFIO/UIO/FUSE)**. → `D-DRV-LINUX-KERNEL-ABI-PINNING`, `D-DRV-LINUX-GPL-SYMBOL-GATING`.

---

## 5. `IBoot` — bootable artifacts

> Requested scope: *"DSS Axis will have libs to create bootable executables, that can be default ones, so interface too: IBoot (UEFI, or any custom that anyone can create) so it's bootable when machine starts, good to create OSs for PCs or specific hardwares."*

### 5.1 The interface

`IBoot` is the contract between **firmware/reset** and **a payload**. It is not a bootloader; it is the shape a bootloader implements.

```
IBoot
  entry(BootContext) -> !            the vehicle hands us the machine
  BootContext:
    memoryMap        physical memory regions + attributes
    console          earliest possible text/serial output
    storage          read blocks from the boot medium
    firmwareServices vehicle-specific handle (EFI_SYSTEM_TABLE, DT blob, …)
  handoff(Payload) -> !              transfer control; never returns
                                     ← WHICH payload is CONFIGURED, §5.3
```

The **vehicle** (B1) supplies `BootContext`; the **payload** (B2) is the thing DSS Axis authors. The DSS OS's kernel is one payload; a chainloader is another; a firmware self-test on custom hardware is another. **`Payload` is never compiled in — it is bound by config (§5.3), because the whole point is that the user chooses the kernel.**

### 5.2 UEFI — the shipped default

**DOCUMENTED.** A UEFI application is **PE32+** with `Subsystem = 10` (`EFI_APPLICATION`); `11` and `12` are boot-service and runtime drivers. Machine types: x64 `0x8664`, AArch64 `0xAA64`. Entry: `EFI_STATUS efi_main(EFI_HANDLE, EFI_SYSTEM_TABLE*)`. On x86_64 the calling convention is **Microsoft x64**; on AArch64, AAPCS64. There is **no import table** — every service is reached through the system table pointer. Images are loaded at a firmware-chosen address and therefore **must carry base relocations**.

★ **Why this is the cheapest possible first target — MEASURED, 2026-08-12.** DSS already emits: PE32+ images; the Microsoft x64 calling convention; `.reloc` base relocations (`IMAGE_REL_BASED_DIR64`, because the exec format declares `DYNAMIC_BASE`); and AArch64 code. The deltas to a booting UEFI image are **three config-and-spine facts, not a backend**: `subsystem: 10`, a non-hosted spine (no CRT, no `exit` import — §1.1), and a new entry verb (§6.3). This is why §1.1 does not gate on Axis and why §12 sequences boot first.

### 5.3 The payload binding — pointing the boot image at the user's kernel

A boot vehicle is a **loader**: its whole job is to end by handing control to **the payload the user chose**. Which payload that is must be **configured, never compiled in** — and the payload is a **separate build artifact**: either *another executable*, or *a library linked into the image*.

Two mechanisms, declared the way this repo already declares `processExit.mechanism` — a closed enumerator, per-mechanism data, and an engine arm each:

| Mechanism | The payload is | Handoff | Fits |
|---|---|---|---|
| `linked-symbol` | a **static library / relocatable objects**, linked *into* the boot image | a direct call to a named symbol, resolved at **link time** | unikernels, `flat-reset` images, *"specific hardwares"* — one file, hermetic |
| `loaded-image` | **another executable**, a separate file on the boot medium | the vehicle **reads, relocates and jumps** at **run time** | PCs, where kernel and bootloader version independently |

```jsonc
// (B) the kernel is linked in — config names the symbol
"bootPayload": { "mechanism": "linked-symbol", "entrySymbol": "dss_kernel_main" }

// (A) the kernel is a separate artifact — config names it
"bootPayload": {
  "mechanism": "loaded-image",
  "path": "/EFI/dss/kernel",
  "format": "elf64-x86_64-baremetal",   // resolves against `format.name`, never a file path
  "handoffProtocol": "dss-boot-v1"
}
```

★ **Where each half lives — the intersection rule, again.** Which payload *this user* wants is **not a property of the object format**. The format declares `bootPayloadMechanisms[]` — *which mechanisms it can realize* — and the **project** picks one and names the payload. This is exactly the `entryVerbs` split (**MEASURED**: *"the accepted entry set is an intersection, and this file owns only half of it"*), and it reuses [`plan-27`](./27-gui-plan.md) §16.4's convention that a payload reference resolves against a format **name**, never a path, so name references survive file moves. → `D-BOOT-PAYLOAD-BINDING`.

**`linked-symbol` is nearly free with today's machinery** (**MEASURED**): the `staticlib` formats already declare `container: "archive"`, and every image format already carries a top-level `entryPoint` naming a symbol resolved against the linked module. `loaded-image` is the one that needs new subsystems (§5.3.4).

#### 5.3.1 Three different things are called "the entry point"

Named once so they are never conflated:

| Entry | Whose | Mechanism | Status |
|---|---|---|---|
| **firmware entry** | the vehicle's own | `efi-main` verb, or offset 0 | §6.3 — existing machinery, new verb |
| **payload entry** | the kernel's | `bootPayload` | **new — this section** |
| **program entry** | a hosted process's `main` | the `entryVerbs` intersection | **absent by construction** — a boot image starts no process |

#### 5.3.2 The handoff protocol must be versioned, and checked twice

Under `loaded-image` the vehicle and the kernel are **separately built artifacts**. Whatever crosses the handoff — memory map, device-tree pointer, `EFI_SYSTEM_TABLE`, a multiboot2 info structure, or `IBoot`'s own `BootContext` — is an **ABI between two binaries no linker ever saw together**. A silent mismatch is a triple fault with no diagnostic.

So `handoffProtocol` is an identifier **plus a version**, checked at **build time** (both artifacts declare it; a mismatch is a build refusal) *and* at **run time** (a magic + version word the vehicle verifies **before** jumping). → `D-BOOT-HANDOFF-PROTOCOL-VERSIONED`.

#### 5.3.3 ★ "Fail loud" has to be redefined for this tier

Everywhere else in DSS, failing loud means a diagnostic on stderr and a non-zero exit. **A bootloader has neither** — no OS, no stderr, no exit code, no parent to report to.

**Jumping to a payload that failed validation is the boot-tier equivalent of a silent miscompile**, and it is the worst outcome available here, because it surfaces as a reset loop with no evidence. The rule: on *any* payload failure — not found, wrong format, version mismatch, failed verification — the vehicle **reports on the earliest console it has** (UEFI `SIMPLE_TEXT_OUTPUT`, a serial port, or the framebuffer) and **halts deliberately**. It never jumps. → `D-BOOT-EARLY-FAILURE-CONSOLE`.

#### 5.3.4 The runtime image reader already has an owner

`loaded-image` requires the vehicle to **parse an object format at run time** — headers, segments, relocations, permissions. DSS already *writes* every one of these formats, so the reader is the mirror of a writer it already owns.

★ It also already has roadmap anchors: `D-FF1-{PE,MACHO,AR}-READER` were registered for the FFI binary readers ([`plan-11`](./11-ffi-plan%20-%20tbd.md)). **One reader, two consumers** — the FFI import path and the boot loader — never two implementations of the same parse. → `D-BOOT-IMAGE-READER-SHARED-WITH-FFI`.

#### 5.3.5 ⚠ `loaded-image` punches a hole through §5.5 unless the chain is verified

Stated here rather than discovered later. Under `loaded-image` the vehicle **loads and executes an arbitrary file at boot**. If the payload path is writable by an unprivileged user, **the vehicle is a bootkit loader** — and one that DSS shipped.

The §5.5 security block therefore covers the **whole chain**, not just the first image:

1. The vehicle **verifies the payload before jumping** — a hash pinned at build time, or a signature checked against a key baked into the vehicle. Failure routes to §5.3.3, never to a jump.
2. §5.5b's system-privilege requirement covers the **payload and its path**, not only the vehicle image.
3. Under Secure Boot the entire chain must be verified, not merely the first link — which is precisely *why* shim verifies GRUB and GRUB verifies the kernel.

`linked-symbol` does not have this exposure: there is one signed artifact and no runtime file to substitute. That asymmetry is a reason to prefer it wherever the deployment allows. → `D-BOOT-PAYLOAD-CHAIN-VERIFICATION`.

#### 5.3.6 Chain-loading is expressible

A vehicle whose payload is *itself* a vehicle (shim → bootloader → kernel) falls out of `loaded-image` with no new mechanism — required by *"any custom that anyone can create"*. Each link owes §5.3.2's version check and §5.3.5's verification to the next.

### 5.4 Custom vehicles — "any custom that anyone can create"

`bootVehicle` is an **enumerator that arrives with its engine arm** — [`plan-27`](./27-gui-plan.md) §16.3's rule, applied verbatim: the object format declares *which vehicles it can realize* (data); the handoff contract and the reset-state machine are **engine only, never data**.

| Vehicle | Artifact shape | Handoff |
|---|---|---|
| `uefi` | PE32+, subsystem 10/11/12 | `efi_main(handle, systab)` |
| `multiboot2` | ELF with a multiboot2 header | loader-supplied info structure |
| `uboot` | ELF or FIT image | device-tree pointer |
| `flat-reset` | **headerless raw image** (e.g. AArch64 `kernel8.img`) | execution begins at offset 0 |
| `mbr` | 512-byte flat, `0xAA55` signature | real-mode, 16-bit |
| `dssos` | *ours to define* | the destination |

`flat-reset` and `mbr` are why the `container` vocabulary needs a third value (§6.2, §9.4): they have **no container headers at all**.

### 5.5 ★ The security block — system-permissioned, never user-permissioned

> Requested constraint: *"this IBoot execution must not be executed when an OS is running — requires system permissioning, not user permissioning — must be a security block."*

**The threat model, stated so the gate can be judged against it:** a toolchain that can *produce and install* pre-OS code is, if its permission model is sloppy, a bootkit factory. The gate exists so that **no unprivileged user can use DSS as a persistence mechanism**. This is a defensive constraint on our own toolchain, and it has two independent halves that must not be confused.

#### (a) Structural — the compiler's half, and it is enforceable

A `boot` artifact must be emitted in a shape **no hosted OS loader will execute as a user process**. Enforced as a **load-time config gate**, fail-loud, in the same shape as the existing profile↔format gate (`D_ArtifactProfileFormatMismatch`, [`plan-06`](./06-artifact-profile-plan%20-%20tbd.md) AP3):

A format declaring `boot` in `artifactProfiles[]`:

1. **may not also declare `cli` or `gui`** — the profiles are mutually exclusive by construction;
2. **may declare no hosted spine** — no `processArgs`, and `processExit` must name a non-hosted mechanism (§6.3);
3. **may name no `cLibrary` runtime-library role** — a boot image links no libc;
4. **must declare a `bootVehicle`**, and the vehicle's shape must be one the OS loaders structurally refuse:
   - UEFI subsystems 10/11/12 are **not** Windows subsystems — the Windows loader will not launch them (**DOCUMENTED — probe before relying on it**);
   - `flat-reset` / `mbr` have no recognized header at all;
   - ⚠ **the ELF vehicles are the weak cell and must be said out loud.** A well-formed `ET_EXEC` ELF with a valid entry *will* be executed by a Linux loader. For `multiboot2` / `uboot` the structural property must be established deliberately (a non-hosted `EI_OSABI`, no `PT_INTERP`, and a probe that the loader actually refuses it) — **or the claim must be narrowed to the vehicles where it holds.** A gate that is assumed rather than measured is a gate that asserts nothing. → `D-BOOT-ARTIFACT-NOT-HOSTED-EXECUTABLE`.

Red-on-disable is available and must be demonstrated: flip a `boot` format to also declare `cli`, and the gate must refuse it at load, naming the file.

#### (b) Deployment — a privilege the toolchain must demand, never grant

Installing a boot artifact — writing the ESP, creating a boot entry (`efibootmgr` / `bcdedit`), flashing SPI/eMMC, enrolling a Secure Boot key — is a **system-privileged** operation. The standing rules:

1. **The compiler driver never performs a privileged install implicitly.** Building a boot artifact writes a file into the output directory and nothing else, ever.
2. Installation is a **separate, explicitly-invoked verb** that (i) requires an explicit privileged credential or elevation, (ii) **refuses to run unprivileged with a real diagnostic** naming what privilege is missing and why, and (iii) **never elevates itself** — no silent UAC prompt, no setuid path, no "convenience" re-exec.
3. **The compiler never emits an artifact that self-installs.** A `boot` payload that writes to the ESP is a build-time refusal.
4. **DSS ships no Secure Boot bypass.** Enrolling a custom key is a firmware-setup operation the *machine owner* performs; DSS's role ends at producing an artifact that *can* be signed by a key the owner already controls (§6.5). If a workflow requires disabling Secure Boot, DSS says so and stops.

→ `D-BOOT-INSTALL-REQUIRES-SYSTEM-PRIVILEGE`, `D-BOOT-NO-SELF-INSTALLING-ARTIFACT`, `D-BOOT-SECUREBOOT-KEY-ENROLLMENT`.

> **Why (a) and (b) are both needed.** (a) alone stops an artifact being *run* on a live OS but not being *installed* for next boot. (b) alone is a policy that a mis-emitted artifact could sidestep. Together they say: the thing cannot run now, and putting it where it *would* run requires a privilege the toolchain refuses to fabricate.

### 5.6 What `IBoot` is not

It is not a claim that DSS can boot arbitrary hardware. Firmware, board bring-up, and the reset state machine are per-machine facts. `IBoot` is the *contract*; a vehicle for a given machine is work.

---

## 6. Artifacts, formats, profiles — extending the existing vocabulary

**MEASURED.** Everything below extends axes that already exist. Nothing invents a parallel mechanism.

### 6.1 New artifact profiles: `driver`, `boot`

`kRegisteredArtifactProfiles` (`src/core/types/artifact_profile.hpp:27`) currently holds `cli, gui, lib, staticlib, script, sproc, transpile, shader, hdl`. Two names are added.

Notably, [`plan-06`](./06-artifact-profile-plan%20-%20tbd.md) §6 Q3 already anticipated this plan **by name** — *"what if a future plan adds `kernelmod` or `wasm`?"* — and resolved it: *"registered set, not compile-time enum … adding a profile = the corresponding backend plan ships + registers."* This is that backend plan.

★ **Execution mode is NOT a profile.** There is no `kdriver` / `udriver` split. A KMDF `.sys` and a UMDF DLL both serve `driver`; **which one you get is a property of the format you selected**, exactly as `cli` is served by four different exec formats today. This preserves the standing veto: *the engine never branches on a profile name* (`artifact_profile.hpp:19-24`). → `D-DRV-EXECUTION-MODE-AXIS`.

### 6.2 New object formats

Following the shipped `<container><bits>-<arch>-<os>-<kind>` convention. **Proposed names, not committed spellings:**

| Format | Serves | Shape |
|---|---|---|
| `pe64-x86_64-windows-sysdrv` / `pe64-arm64-…` | `driver` | PE, subsystem 1, `DriverEntry`, imports `ntoskrnl`/`hal` |
| `pe64-x86_64-windows-umdrv` | `driver` | PE DLL, UMDF host |
| `elf64-x86_64-linux-kmod` / `elf64-aarch64-…` | `driver` | ELF `ET_REL` + `.modinfo` |
| `macho64-arm64-darwin-dext` | `driver` | Mach-O in a `.dext` bundle |
| `pe64-x86_64-uefi-app` / `pe64-aarch64-uefi-app` | `boot` | PE32+, subsystem 10 |
| `pe64-x86_64-uefi-bootdrv` / `-rtdrv` | `boot` | subsystems 11 / 12 |
| `flat-aarch64-baremetal-img` | `boot` | **headerless** |
| `elf64-x86_64-multiboot2` | `boot` | ELF + multiboot2 header |

★ **The `-os-` slot is really a *platform ABI* slot, and `uefi` proves it.** UEFI is firmware, not an operating system. The existing vocabulary already tolerates this (**MEASURED**: `ObjectFormatKind` is documented as a *container* enum where `Elf` covers Linux *and* Android, `MachO` covers macOS *and* iOS — OS is a per-format fact, never an engine discriminator). Adding `uefi` and `baremetal` costs no new axis. This is a genuine strength of the existing design and should be recorded as such.

**Three new mechanisms are needed:**

- **`container: "flat"`** — a third `ObjectFormatContainer` value (today `Single | Archive`, `object_format_schema.hpp`). A raw reset-vector image has no container headers whatsoever. → `D-BOOT-FLAT-CONTAINER`.
- **`driverVehicle` / `bootVehicle` enumerators**, each with its engine arm, per [`plan-27`](./27-gui-plan.md) §16.3. → `D-DRV-VEHICLE-ENUMERATOR`, `D-BOOT-VEHICLE-ENUMERATOR`.
- **`bootPayloadMechanisms[]`** on the format (which payload bindings it can realize) paired with the project-side `bootPayload` selection (§5.3) — the same format-owns-half intersection as `entryVerbs`. → `D-BOOT-PAYLOAD-BINDING`.

### 6.3 New entry verbs

**MEASURED.** `EntryMaterialization` (`src/core/types/entry_shape.hpp:167`) is a closed enum — `None | ArgcArgv | ArgcWargv` — and each verb **arrives with an engine arm**. The format declares the verbs it can realize; the *language* declares each entry name's signature and verb; a candidate must survive **the intersection**. That design is exactly right for this plan and needs no change in shape, only new values:

| Verb | Signature | Realized by |
|---|---|---|
| `driver-entry` | `(PDRIVER_OBJECT, PUNICODE_STRING) -> NTSTATUS` | `*-sysdrv` |
| `module-init` | `() -> i32`, paired `module-exit` | `*-kmod` |
| `efi-main` | `(EFI_HANDLE, EFI_SYSTEM_TABLE*) -> EFI_STATUS` | `*-uefi-*` |
| `none` *(exists)* | — | `flat-*` — execution begins at offset 0 |

→ `D-DRV-ENTRY-VERBS`, `D-BOOT-ENTRY-VERBS`.

### 6.4 New runtime-library roles

**MEASURED.** `RuntimeLibraryRole` is `None | CLibrary | UnwindPersonality | SystemPrimitives`, and the comment at `object_format_kind.hpp` explicitly anticipates growth: *"adding a fourth role is an enum slot + a JSON row and touches no reader."* Needed: **`kernelLibrary`** (`ntoskrnl.exe` / `hal.dll` / the kernel's exported symbol set / DriverKit frameworks) and **`driverFramework`** (`wdfldr.sys`).

⚠ **UEFI needs the opposite — the empty table.** A UEFI image names *no* runtime library. The bidirectional `validate()` rule (every claimed role resolves; every declared row is claimed) must remain satisfiable when the table is **empty and nothing claims anything**. Verify this is expressible before relying on it. → `D-BOOT-NONHOSTED-SPINE`.

### 6.5 Packaging and the three external signing authorities

A driver is rarely one file. Per [`plan-27`](./27-gui-plan.md) §16.4's finding — *a package descriptor's `inputs` is a list, so no single object format can own the container* — driver and boot packaging belongs in a **sibling `package-formats/` schema**, which plan 27 already proposed and which does **not exist in the tree today** (**MEASURED**). Whichever of 27 or 28 lands first authors it; the second reuses it.

| Package | Contents |
|---|---|
| Windows driver package | `.sys` + INF + catalog (`.cat`) |
| macOS system extension | `.dext` bundle inside a host app's `Contents/Library/SystemExtensions` |
| Linux module | `.ko` (+ signature appendix) |
| ESP image | FAT filesystem containing `\EFI\BOOT\BOOT{X64,AA64}.EFI` |

★ **Three gates that are not engineering problems** — named the way [`plan-27`](./27-gui-plan.md) §10.2 named Apple provisioning, so nobody plans around them:

1. **Windows kernel drivers** need an **EV code-signing certificate** and submission through Microsoft's hardware dev portal (attestation or WHQL). Test-signing needs `bcdedit /set testsigning on` on the target machine. → `D-DRV-WINDOWS-ATTESTATION-SIGNING`.
2. **macOS DriverKit** needs `com.apple.developer.driverkit` **plus a family entitlement**, which Apple **grants by application**. No amount of toolchain work substitutes. → `D-DRV-DARWIN-DRIVERKIT-ENTITLEMENT`.
3. **UEFI Secure Boot** requires a key in `db` — either a signature from an authority the firmware already trusts, or a custom key the **machine owner** enrolls in firmware setup (§5.5b). → `D-BOOT-SECUREBOOT-KEY-ENROLLMENT`.

---

## 7. The freestanding profile — what a driver and a boot image cannot have

This section exists because it is the cross-plan lock (§9.1), and because getting it wrong is silent.

| Facility | Hosted program | Kernel driver | Boot image |
|---|---|---|---|
| libc | yes | **no** | **no** |
| GC | ★ **none anywhere** — [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §2.3 removed it | **n/a** | **n/a** |
| Ownership / reclamation | T0–T4 lattice ([`plan-09.5`](./09.5-dss-hir-plan.md) §4) | ✅ **yes** — compile-time, zero runtime cost | ✅ **yes** |
| Exceptions / unwinder | yes | **no** (kernel unwind is the OS's) | **no** |
| Async scheduler | the default execution model | **no** | **no** |
| Implicit heap allocation | yes | **only in sleepable context** (§3.2) | **no** — before a heap exists |
| Floating point | yes | **restricted** (must be explicitly saved/restored on Windows) | restricted |
| Stack | large | **small and fixed** | **whatever the vehicle gave us** |
| `main` + argv | yes | **no entry of that shape at all** | **no** |

★ **This table got substantially better on 2026-08-12, and the reason is worth stating.** It formerly read: *"plan-24 §2.2 makes async 'the default execution model' and §2.3 makes the async GC 'the moat.' Both are unavailable in every artifact this plan produces."* That was true of the async-GC design. It is **no longer true of memory**: [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §2.3 replaced the collector with **proven-static reclamation**, which is a *compile-time analysis* — so a driver and a boot image get **full automatic memory management at zero runtime cost**, the one thing this table originally had to strike out.

What remains unavailable is only what genuinely needs linked code: the **scheduler, unwinder and libc**. So the requirement stands but has shrunk to its honest core — the runtime must be a **declared, subtractable set**, and [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §3.1a's independent `memory` / `runtime.enable` blocks are exactly that. See §9.1.

⚠ **`@manual` is not the freestanding mechanism**, and the annotation's earlier proposed spelling (`@kernel`) actively invited that confusion — which is why [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §3.1b renamed it, citing this plan's own use of *kernel* among the collisions. A driver does **not** need `@manual`; it needs T0–T3, which are automatic. `@manual` is for the un-inferable case — a DMA buffer whose lifetime the hardware owns.

---

## 8. Verification — the VM leg

**Decision: a real VM execution leg**, because a driver or boot image that has only been *emitted* has not been shown to work, and BUILD-only pins would let this plan claim more than it earned.

**MEASURED** — the corpus harness already has the mechanism half: `expected.json` carries per-target `runOn` (host-OS gate) and `emulator` (today `qemu-aarch64`), consumed by `tests/examples/examples_runner.cpp`. A VM leg is a new manifest axis (guest firmware / kernel / disk image / serial capture / timeout), not a new harness concept.

⚠ **It must land in BOTH runners.** `tests/examples/examples_runner.cpp` (in-process) *and* `integrated_tests/runner.cpp` (CLI-subprocess). One enforcing while its sibling shrugs is a silent harness bug of exactly the shape this repo has been bitten by before.

### 8.1 Coverage, stated honestly

| Leg | Mechanism | Verdict |
|---|---|---|
| **UEFI** | **QEMU + OVMF**, FAT ESP containing `BOOTX64.EFI`, output over serial, status as exit | ✅ **fully automatable, no guest license, no image build** — by a wide margin the cheapest real-execution leg in this plan |
| **Linux `.ko`** | QEMU + a minimal kernel + initramfs; `insmod`; read `dmesg` over serial | ✅ automatable. ★ The guest kernel and the build's `vermagic` input are **the same kernel** — which makes the VM leg *also* the mechanism that makes §4.3's per-kernel build honest |
| **virtio device** | QEMU's own virtio devices — no hardware needed | ✅ the natural first bus (§3.3) |
| **Windows `.sys`** | licensed Windows guest, test-signing enabled | ⚠ heaviest leg; licensing + provisioning are real costs |
| **macOS `.dext`** | ✘ cannot be virtualized off Apple hardware; needs a real Mac, user approval, and an Apple-granted entitlement | ✘ **BUILD-only until the entitlement exists** — an **external gate, anchored, not a shrug** → `D-DRV-DARWIN-VM-LEG-EXTERNALLY-GATED` |

The uncovered cell is **named**, not silently dropped — the standing rule that a bounded coverage decision must be logged rather than presented as completeness. → `D-DRV-VM-LEG-HARNESS`, `D-BOOT-OVMF-VM-LEG`.

### 8.2 Red-on-disable

Every leg must have a demonstrated failure arm, exercised rather than reasoned about: corrupt the emitted `.modinfo` vermagic → `insmod` must fail and the leg must go red; flip the UEFI subsystem to 3 → firmware must refuse the image. **A gate that has never been seen to fail has not been shown to be a gate.**

---

## 9. Architectural locks — what the engine must NOT foreclose

**This is the section that has value today**, while both halves are reserved. Same role as [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §4.

1. **★ The runtime must be subtractable — ✅ MECHANISM LANDED 2026-08-12.** DSS Axis's unwinder, scheduler and allocator must be a **declared set a target can decline**, never an assumed floor. *Formerly this lock also named the GC and asked that the mechanism be invented; [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §3.1a supplied it instead* — two **independent** config blocks, `memory` (compile-time) and `runtime.enable` (linked services), so declining the runtime is a config fact rather than a special case. **The residual obligation is a pin, not a design:** the day any lowering *requires* a runtime service to emit correct code for an ordinary function, drivers and boot images become inexpressible — which is why [`plan-09.5`](./09.5-dss-hir-plan.md) `D-DSSHIR-DISABLED-PATH-COMPLETE` must be **tested**, not assumed. → `D-DRV-FREESTANDING-LANGUAGE-PROFILE`.
2. **A format must be able to declare no C library.** The `runtimeLibraries` bidirectional validation must stay satisfiable with an **empty** table, and the spine blocks must accept a non-hosted exit mechanism. → `D-BOOT-NONHOSTED-SPINE`.
3. **Entry verbs stay open and stay an intersection.** Nothing may assume an entry returns `int` to a process-exit path, or that argc/argv are concepts that exist. The existing verb design already satisfies this; it must not regress. → `D-DRV-ENTRY-VERBS`.
4. **`container` must admit a headerless image.** `Single | Archive` cannot express a reset-vector image. → `D-BOOT-FLAT-CONTAINER`.
5. **Position-independent image emission must be preserved.** UEFI images are relocated by firmware. **MEASURED**: PE `.reloc` emission exists today — a future change that made `.reloc` conditional on a Windows-only predicate would silently foreclose UEFI. → `D-BOOT-PE-BASE-RELOCATIONS`.
6. **No privileged action may ever be implicit** (§5.5b). The toolchain never writes an ESP, enrolls a key, loads a driver, or elevates itself. → `D-BOOT-INSTALL-REQUIRES-SYSTEM-PRIVILEGE`.
7. **Agnosticism (Decision #4).** No `if (os == "windows")` / `if (vehicle == "uefi")` in shared substrate. Vehicles are **config-declared enumerators with engine arms**, exactly like plan 27's `windowVehicle`: the format declares *which vehicles it can realize*; the dispatch contract and lifecycle live in the engine and are **never data**.
8. **Host services lower through a closed intrinsic vocabulary.** MMIO ordering, barriers, and DMA sync must live in HIR/MIR as a **closed intrinsic set** (the same discipline [`plan-24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) §4.8 applies to atomics), so `IDriverHost` lowers config-driven to ISA operations — never per-OS concurrency C++. → `D-DRV-HOST-SERVICES-CONTRACT`.
9. **The execution-context effect must be expressible in the effect system** (§3.2). Retrofitting a context discipline after drivers exist means auditing every call site by hand. → `D-DRV-EXECUTION-CONTEXT-EFFECT`.

---

## 10. Open questions (deferred until triggered)

| # | Question |
|---|----------|
| 1 | Where does the `IOSDriver` family physically **live** — DSS Axis stdlib source, an HIR-level contract, or a `.lang.json`-declared vocabulary? (§3.1 assumes stdlib source; unconfirmed.) |
| 2 | Is the DSS OS driver model **defined by** this contract (zero-shim) or **adapted to** it like every other OS? §4.1 argues the former; it is the author's call. |
| 3 | Linux: per-kernel builds (DKMS-shaped), Android **GKI stable KMI** only, or **user-mode only**? (§4.3) |
| 4 | **Licensing, a human decision:** `EXPORT_SYMBOL_GPL` + `MODULE_LICENSE("GPL")` place a GPL obligation on driver *source*. Does DSS emit the declaration, refuse to, or require it explicitly? |
| 5 | Does `IGraphicsDriver` stay display-only (§3.1), or is accelerated 3D a separate plan? |
| 6 | Does `package-formats/` land here or in [`plan-27`](./27-gui-plan.md)? (Neither exists today — **MEASURED**.) |
| 7 | Diagnostic-code prefix for this tier. `D_*` is the compiler driver's (§0.1); `B_*` is debug-info's. |
| 8 | Does `IBoot` open **before** the driver half? §12.1 argues strongly yes. |
| 9 | iOS/iPadOS DriverKit — is there a reachable cell at all, or does the matrix (§4.1) declare it permanently out of scope? |
| 10 | Which payload mechanism (§5.3) is the **DSS OS default** — `linked-symbol` (one signed artifact, no §5.3.5 exposure) or `loaded-image` (kernel and bootloader version independently)? A general-purpose OS usually needs the latter; the security asymmetry argues for the former wherever deployment allows. |
| 11 | Does `handoffProtocol` (§5.3.2) define **`BootContext` as its own wire format**, or does each vehicle pass its native structure (`EFI_SYSTEM_TABLE`, DT blob, multiboot2 info) with `BootContext` as a thin view over it? The latter is cheaper; the former is what makes a kernel vehicle-portable. |

---

## 11. Cross-plan obligations

Recorded, not silently assumed. **None of these edits has been made** — each is a proposal this plan owes its neighbours:

| Plan | Obligation |
|---|---|
| [`24`](./24-dss-axis-language-reserved-plan%20-%20tbd.md) | ✅ **DONE 2026-08-12** — §3.1a's independent `memory` / `runtime.enable` blocks realize lock #1. ⏳ **still owed:** §3.3 gains the **execution-context effect** (§3.2 — `may_sleep` vs atomic context as a compile error). Now cheaper than when first proposed: §3.1b landed annotations-with-parameters, which is the surface it needs. |
| [`09.5`](./09.5-dss-hir-plan.md) | ★ **NEW** — the freestanding cell is `memory` on + `runtime.enable` off; `D-DSSHIR-DISABLED-PATH-COMPLETE` is the pin this plan depends on. The **execution-context effect** (§3.2) is a value-graph property and likely belongs in the DSS-HIR analysis, not in Axis alone. |
| [`21`](./21-runtime-reserved-plan%20-%20tbd.md) | §3's *"inlined per-program vs separate runtime lib"* open question gains a third answer: **absent entirely**. ✅ §2.1's GC module no longer blocks this plan — Axis has no collector. |
| [`06`](./06-artifact-profile-plan%20-%20tbd.md) | §3 profile table gains `driver` + `boot`; §6 Q3's `kernelmod` hypothetical resolves **here** |
| [`27`](./27-gui-plan.md) | `package-formats/` co-ownership (§6.5); `bootVehicle`/`driverVehicle` follow `windowVehicle`'s precedent |
| [`14`](./14-linker-plan%20-%20tbd.md) | new formats, `container: flat`, the non-hosted spine |
| [`11`](./11-ffi-plan%20-%20tbd.md) | the `D-FF1-{PE,MACHO,AR}-READER` binary readers gain a **second consumer** — the boot vehicle's runtime image loader (§5.3.4). One reader, two consumers, never two implementations. |
| [`16`](./16-codesign-publish-plan%20-%20tbd.md) | three new signing authorities (§6.5), plus **payload chain verification** (§5.3.5) — the vehicle must verify what it loads, which is a signing-substrate consumer, not just a signing producer |
| [`00`](./00-compiler-implementation-plan%20-%20tbd.md) | §0.1 roadmap row + the §"Status & sub-plans" bullet index, so `/dss-cycle` can see this plan at all |

---

## 12. Deferred anchors (owned by this plan; register when it opens)

These **35** anchors are **reserved/future** — they live here until the plan opens, then move into [`_deferred-anchor-registry-production`](./_deferred-anchor-registry-production.md) as active four-cell rows. Reserved-plan anchors are not yet cited in `src/`, so the CI anchor-guard does not require registry rows today. Every ID carries ≥3 hyphen-separated segments after `D-`, which is what the guard's `D-[A-Z0-9_]+(-[A-Z0-9_]+){2,}` shape requires to treat it as a real anchor rather than an informal label.

| Anchor | Owns |
|--------|------|
| `D-DRV-OSDRIVER-CONTRACT` | the `IOSDriver` up-call family + the registered device-class vocabulary |
| `D-DRV-HOST-SERVICES-CONTRACT` | `IDriverHost` — MMIO / DMA / IRQ / memory / time / power down-calls |
| `D-DRV-EXECUTION-CONTEXT-EFFECT` | atomic-context vs may-sleep as an effect-system contract, not a comment |
| `D-DRV-BUS-TRANSPORT-AXIS` | bus/transport modelled orthogonally to device class |
| `D-DRV-EXECUTION-MODE-AXIS` | kernel/user co-equal; mode is a format property, never a profile name |
| `D-DRV-VEHICLE-ENUMERATOR` | `driverVehicle` enumerator + its engine arm (never data) |
| `D-DRV-ENTRY-VERBS` | `driver-entry` / `module-init` entry materialization |
| `D-DRV-KERNEL-LIBRARY-ROLE` | `kernelLibrary` + `driverFramework` runtime-library roles |
| `D-DRV-FREESTANDING-LANGUAGE-PROFILE` | ★ the subtractable-runtime lock (cross-plan: 24 + 21) |
| `D-DRV-GRAPHICS-CLASS-SCOPE-LIMIT` | `IGraphicsDriver` is display-only; accelerated 3D is not claimed |
| `D-DRV-LINUX-KERNEL-ABI-PINNING` | vermagic / symbol CRCs / per-kernel build inputs |
| `D-DRV-LINUX-GPL-SYMBOL-GATING` | `EXPORT_SYMBOL_GPL` + `MODULE_LICENSE` — a licensing consequence |
| `D-DRV-WINDOWS-ATTESTATION-SIGNING` | EV certificate + Microsoft hardware-portal submission |
| `D-DRV-WINDOWS-INF-DRIVER-PACKAGE` | INF + catalog packaging |
| `D-DRV-DARWIN-DRIVERKIT-ENTITLEMENT` | Apple-granted DriverKit family entitlements |
| `D-DRV-DEXT-BUNDLE-PACKAGING` | `.dext` bundle inside a host app's system-extension directory |
| `D-DRV-DSSOS-NATIVE-VEHICLE` | the DSS OS driver model, defined to fit the contract natively |
| `D-DRV-VM-LEG-HARNESS` | the VM execution leg, in **both** corpus runners |
| `D-DRV-DARWIN-VM-LEG-EXTERNALLY-GATED` | the named uncovered cell (§8.1) |
| `D-BOOT-IBOOT-CONTRACT` | the `IBoot` interface + `BootContext` |
| `D-BOOT-VEHICLE-ENUMERATOR` | `bootVehicle` enumerator + engine arm; third-party vehicles |
| `D-BOOT-NONHOSTED-SPINE` | a non-hosted `ExitMechanism` + a declinable entry trampoline; the empty `runtimeLibraries` table |
| `D-BOOT-ENTRY-VERBS` | `efi-main`; `none` for a flat reset image |
| `D-BOOT-FLAT-CONTAINER` | `container: "flat"` — a headerless raw image |
| `D-BOOT-PE-BASE-RELOCATIONS` | `.reloc` must stay unconditional (**MEASURED**: exists today) |
| `D-BOOT-ARTIFACT-NOT-HOSTED-EXECUTABLE` | ★ the structural half of the security block (§5.5a), incl. the weak ELF cell |
| `D-BOOT-INSTALL-REQUIRES-SYSTEM-PRIVILEGE` | ★ the deployment half (§5.5b) — system, never user permission |
| `D-BOOT-NO-SELF-INSTALLING-ARTIFACT` | a boot payload that installs itself is a build-time refusal |
| `D-BOOT-SECUREBOOT-KEY-ENROLLMENT` | Secure Boot key trust — owner-enrolled, never bypassed |
| `D-BOOT-PAYLOAD-BINDING` | ★ `bootPayload` — `linked-symbol` vs `loaded-image`; format declares mechanisms, project names the kernel |
| `D-BOOT-HANDOFF-PROTOCOL-VERSIONED` | the vehicle↔payload ABI, versioned and checked at build **and** run time |
| `D-BOOT-EARLY-FAILURE-CONSOLE` | ★ fail-loud at the boot tier — report and halt, **never jump** to an unvalidated payload |
| `D-BOOT-IMAGE-READER-SHARED-WITH-FFI` | one runtime object-format reader, shared with `D-FF1-*-READER`, never two |
| `D-BOOT-PAYLOAD-CHAIN-VERIFICATION` | ★ `loaded-image` verifies the payload before jumping; the security block covers the whole chain |
| `D-BOOT-OVMF-VM-LEG` | QEMU + OVMF execution witness |

---

## 13. Sequencing

**Not sequenced. Reserved.** No cycles until §1 triggers. When a half opens, this file gains a `## 0.1 Stepper` with a cycle-by-cycle log and that half's anchors migrate into the registry.

### 13.1 The recommendation, when it does open: **boot first, and by a wide margin**

This is the plan's most actionable finding and it is deliberately placed where it cannot be missed.

The **boot half is not DSS-Axis-dependent** (§1.1), and the distance from today's engine to a booting UEFI image is **three config-and-spine facts, not a backend** (§5.2) — because PE32+, the Microsoft x64 calling convention, AArch64 codegen, and **`.reloc` base relocations** are all **already in the tree** (**MEASURED 2026-08-12**). Its verification leg (QEMU + OVMF) is the cheapest real-execution leg described anywhere in this plan: no guest license, no image build, no hardware.

So the natural order is:

| Phase | Deliverable | Why here |
|---|---|---|
| **1** | `boot` profile + the §5.5a structural gate + `pe64-x86_64-uefi-app` + `efi-main` verb + non-hosted spine | Reachable with **today's engine + a C-subset payload**. Proves the freestanding spine with no new language. |
| **2** | QEMU + OVMF VM leg, red-on-disable demonstrated | Turns phase 1 from an emitted file into a **witnessed boot**. |
| **3** | `bootPayload: linked-symbol` + `container: flat` + `flat-aarch64-baremetal-img` | Proves the vehicle axis with a maximally-different second vehicle **and** the cheap payload mechanism together — a flat image *is* the linked-symbol case. Nearly free on existing `staticlib` + `entryPoint` machinery. |
| **4** | `bootPayload: loaded-image` — runtime image reader (shared with `D-FF1-*-READER`), versioned handoff, chain verification, early-failure console | The real loader, and the first phase with genuine new subsystems. §5.3.3 and §5.3.5 are the reason this is its own phase and not a footnote on phase 3. |
| **5** | `IBoot` contract in DSS Axis + the §5.5b install-privilege verb | Needs the language; the security half needs a deliberate cycle of its own. |
| **6** | `IDriverHost` + `virtio` bus + Linux `.ko` + the QEMU driver leg | The cheapest driver cell: virtio needs no hardware and the guest kernel is also the build input. |
| **7** | Windows `.sys`, then `.dext` (build-only), then the DSS OS vehicle | Descending order of external gating. |

Phases 1–2 alone would prove the freestanding spine, the non-hosted format shape, the boot security gate, and the VM leg — **four of this plan's nine architectural locks, without DSS Axis existing.** Phases 3–4 add the payload binding, at which point the toolchain can build *and boot* a kernel of the user's choosing. That is an unusually high return for a plan filed as reserved, and it is the strongest argument for §10 Q8 being answered *yes*.
