/* examples/c/windows_module_and_memory_query -- the shipped <windows.h>'s module and memory-query calls, RUN on the
 * program's own image (P69 fold f26d, lane `cs`, P0): its handle by name and by an address inside it, its file
 * name in both encodings, and what a stack page and a code page are. Exit 1..10 names the first check to fail,
 * 42 = all right. The declarations' types and MEMORY_BASIC_INFORMATION's layout are asserted at compile time, so a
 * wrong declaration fails the build on every host. */
#include <windows.h>
#include <stddef.h>

_Static_assert(_Generic(&GetModuleFileNameA, DWORD (WINAPI *)(HMODULE, LPSTR, DWORD): 1, default: 0),
               "GetModuleFileNameA is the SDK's");
_Static_assert(_Generic(&VirtualQuery, SIZE_T (WINAPI *)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T): 1, default: 0),
               "VirtualQuery is the SDK's");
_Static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 48 && offsetof(MEMORY_BASIC_INFORMATION, RegionSize) == 24,
               "MEMORY_BASIC_INFORMATION is the SDK's");

static int inside_this_image(int x) { return x + 1; }

int main(void) {
    HMODULE self = GetModuleHandleA(NULL);
    if (self == NULL) return 1;
    if (GetModuleHandleW(NULL) != self) return 2;
    HMODULE byAddress = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void *)&inside_this_image, &byAddress))
        return 3;
    if (byAddress != self) return 4;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || path[n] != '\0') return 5;
    WCHAR wide[MAX_PATH];
    if (GetModuleFileNameW(self, wide, MAX_PATH) == 0) return 6;
    volatile int local = inside_this_image(41);
    MEMORY_BASIC_INFORMATION stack;
    if (VirtualQuery((LPCVOID)&local, &stack, sizeof stack) != sizeof stack) return 7;
    if (stack.State != MEM_COMMIT || stack.Type != MEM_PRIVATE) return 8;
    MEMORY_BASIC_INFORMATION code;
    if (VirtualQuery((LPCVOID)(void *)&inside_this_image, &code, sizeof code) != sizeof code) return 9;
    if (code.Type != MEM_IMAGE || code.AllocationBase != (PVOID)self) return 10;
    return local == 42 ? 42 : 11;
}
