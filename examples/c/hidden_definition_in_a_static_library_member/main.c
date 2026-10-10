/* The PROGRAM: links the static library the arm's `dependsOn` builds from lib.c and calls the member's hidden
 * function and reads its hidden datum. Exit 42 iff both arrived intact: scale_hidden(20) + offsets_hidden[0] +
 * offsets_hidden[1] == 40 + 1 + 1, with lib_version() == 1 as the member's plain control. */
__attribute__((visibility("hidden"))) int scale_hidden(int x);
extern int offsets_hidden[2];
int lib_version(void);

int main(void) {
    return lib_version() == 1 && scale_hidden(20) + offsets_hidden[0] + offsets_hidden[1] == 42 ? 42 : 1;
}
