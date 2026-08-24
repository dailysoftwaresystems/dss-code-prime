// FC5 — the truly-empty statement `;`. All the loop work lives in the for-update
// (a comma-sequenced pair); the loop BODY is a bare `;`. Pins that an empty
// statement lowers to a no-op (not a parse error, not a stray block) and that an
// empty for-body composes with a comma-operator update. Fold-resistant: runtime
// trip count.
int count(int n) {
    int acc = 0;
    int i;
    for (i = 0; i < n; i = i + 1, acc = acc + 3)
        ;                       // truly-empty body — work is in the update
    return acc;                 // n * 3
}

int main() {
    return count(14);           // 14 * 3 = 42
}
