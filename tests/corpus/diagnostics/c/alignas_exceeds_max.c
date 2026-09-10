/* P63 D-CSUBSET-ALIGNMENT-CEILING-REFUSES-WHAT-TWO-REFERENCES-RUN.
   The corpus runner analyses with NO aggregateLayout params at all, so the
   TARGET-DECLARED `maxRequestedAlignment` is not in scope here; the only bound
   left is REPRESENTABILITY in the `Alignment` newtype, and that is the arm this
   file pins. The declared-ceiling arm is pinned where it can be:
   SemanticAnalyzerC.TheRequestableCeilingComesFromTheDeclaredParams and the
   examples/c/alignment_page_request runtime witness.
   ⚠ THE SUBJECT USED TO BE `alignas(512)` AND IT WAS PINNING A UNION VIOLATION:
   gcc 13.3.0 and clang 18.1.3 both BUILD AND RUN that, measured, both spellings. */
alignas(4294967296) int x;
int main(void) { return 0; }
