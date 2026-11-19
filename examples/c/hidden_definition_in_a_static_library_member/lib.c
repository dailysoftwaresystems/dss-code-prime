/* The ARCHIVE MEMBER: a hidden function and a hidden datum that nothing in this member uses — the program that links
 * the archive does. A static library's hidden definitions are internal to the IMAGE it is linked into, not to the
 * member: the link that pulls the member resolves them by name. */
__attribute__((visibility("hidden"))) int scale_hidden(int x) { return x * 2; }
__attribute__((visibility("hidden"))) int offsets_hidden[2] = {1, 1};

/* The member's one exported entry point, so the archive is pulled for a reason of its own as well. Its loop is there
 * for the optimizer to fold, so the release build of this member is a different image from the baseline one — which
 * is what lets the release arm prove the member WAS optimized and still kept the hidden pair above. */
int lib_version(void) {
    int sum = 0;
    for (int i = 0; i < 4; ++i) sum += i;
    return sum == 6 ? 1 : 0;
}
