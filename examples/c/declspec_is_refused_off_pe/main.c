/* `__declspec(...)` OFF WINDOWS IS REFUSED BY NAME, ONCE PER SPECIFIER.
 *
 * The spelling exists for the pe object format only. gcc for Linux has no such
 * word (a syntax error) and clang for Linux and macOS refuses it by name; here
 * the language document says which formats the spelling is available for, and
 * anywhere else a specifier written in it is refused before any modifier in it
 * is read — whatever the modifier, and whether or not this compiler could have
 * honoured the same request under its plain spelling.
 *
 * It is refused rather than ignored for the reason the spelling stopped being
 * erased on Windows: `__declspec(align(32))` and `__declspec(thread)` change
 * what the program means, and a compile that drops them in silence produces a
 * different program than the one that was written.
 *
 * The same two requests in the plain spelling compile for every target
 * (`__attribute__((noinline))`, `__attribute__((aligned(32)))`), which is the
 * control: it is the SPELLING that is unavailable here, not the requests.
 */
__declspec(noinline) int one(void) { return 1; }

__declspec(align(32)) int aligned_object;

int main(void) { return one() + aligned_object; }
