// The canonical "compile + run + exit N" smoke. Pins the entire
// c → PE-Exec pipeline: tokenizer → parser → semantic →
// HIR → MIR → LIR → regalloc → callconv → assembler → linker →
// trampoline injection → image emission → OS spawn. A break
// anywhere in the chain surfaces here as a wrong exit code.
int main() {
    return 42;
}
