// D-CSUBSET-PACKED-ATOMIC-MEMBER — the runtime witness for an UNDER-ALIGNED
// `_Atomic` scalar access.
//
// `g.a` is an `_Atomic int` at byte offset 1 of a `__attribute__((packed))`
// struct, so the access cannot be naturally aligned. Before P53 the compiler
// emitted the target's inline atomic pair for it (`stlr`/`ldar` on arm64), and
// that binary ✔MEASURED `rc 135, Bus error (core dumped)` on a NATIVE aarch64
// host. It now routes through the format's declared atomics runtime
// (`__atomic_store` / `__atomic_load`, the GENERIC runtime-sized entries).
//
// ⚠⚠ THE RUN VERDICT ON THREE OF THE FOUR ARMS BELOW PROVES ONLY THAT NOTHING
// BROKE — NOT THAT THE DEFECT IS FIXED. qemu-user does not enforce the
// LDAR/STLR alignment check and x86-64 tolerates the misaligned access, so the
// PRE-FIX binary also exits 42 on qemu, on Windows and in WSL. The arms that
// can actually see this defect are the `darwin` arm (native Apple Silicon,
// `emulator: ""`) and a run on real aarch64 hardware. The compile-time half —
// which access takes the libcall and which keeps the native instruction — is
// pinned where it is visible on every host, in `tests/lir/test_mir_to_lir`.
//
// `h.a` is the ALIGNED CONTROL and it is the half that makes this example say
// something. Same type, same access, naturally aligned: it must KEEP the native
// inline form. A "fix" that routed every atomic through the runtime would pass
// the fault test and still be wrong, and only a control catches that.
struct __attribute__((packed)) P { char pad; _Atomic int a; };
struct Q                       { char pad; _Atomic int a; };

struct P g;
struct Q h;

int main(void) {
    g.a = 7;             // under-aligned     -> the atomics-runtime store
    h.a = 35;            // naturally aligned -> the native store
    int const x = g.a;   // under-aligned     -> the atomics-runtime load
    int const y = h.a;   // naturally aligned -> the native load
    return x + y;        // 7 + 35 = 42
}
