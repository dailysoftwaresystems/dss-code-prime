/* The LAST LINE of this file is a `#define` whose `//` comment runs to the
 * end of the input: NO newline follows it (P68 round 8,
 * D-C-LINE-COMMENT-AT-END-OF-FILE-REFUSED). gcc 13.3.0, clang 18.1.3,
 * aarch64 gcc, mingw-w64 gcc 13.2.0 and MSVC 19.51 all accept it; DSS
 * refused it with P_UnterminatedComment until C's `line-comment` mode
 * declared `atEndOfInput: closes`. Two frames are open at the end of input
 * here, the directive's and the comment's, and both must close. */
#define ANSWER 42

int main(void) {
    return ANSWER;
}
#define LAST_LINE 1 // the file ends inside this comment; no newline follows it