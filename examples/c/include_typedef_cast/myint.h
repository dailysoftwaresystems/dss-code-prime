// The include-delivered typedef: invisible to main.c's FIRST parse
// (files parse alone; trees merge post-parse), so the ambiguous cast in
// negate() can only commit via the CU type-name oracle's reparse.
typedef int myint;
