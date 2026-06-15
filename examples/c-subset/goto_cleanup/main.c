// FC5 — `goto` to a shared cleanup/exit label: the canonical C error-handling
// idiom (forward goto over a cleanup block + a `done:` join). Fold-resistant:
// the seeds arrive as runtime function args, so the CondBr-to-`fail` edges stay
// live and the optimized arm keeps real unstructured control flow.
//
// On the (19, 23) path neither guard fires, so control reaches `goto done` and
// SKIPS the `fail:` block — `acc` stays 42. A broken goto that fell into `fail`
// (or failed to skip it) would overwrite `acc` with 1000 — the red-on-disable
// lever this corpus pins through the whole source -> binary chain.
int run(int a, int b) {
    int acc = 0;
    if (a < 0) goto fail;       // a = 19 -> not taken
    acc = acc + a;              // acc = 19
    if (b < 0) goto fail;       // b = 23 -> not taken
    acc = acc + b;              // acc = 42
    goto done;                  // forward goto over the cleanup block
fail:
    acc = 1000;                 // dead on the (19, 23) path
done:
    return acc;                 // 42
}

int main() {
    return run(19, 23);
}
