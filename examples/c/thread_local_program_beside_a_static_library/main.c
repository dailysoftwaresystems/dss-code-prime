/* A program with a thread-local of its own, linked beside a static library's member (see expected.json).
   42 = t (35, read on this thread) + shared (7, read through the member) — 0 or a fault if either read is wrong;
   a different value if t's or shared's read reaches the wrong object. */
_Thread_local int t = 35;
int shared = 7;

int read_shared(void);

int main(void) { return t + read_shared(); }
