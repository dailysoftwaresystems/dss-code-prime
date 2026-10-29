/* C11/C23 call_once on the UCRT's platform — <threads.h>'s, and since C23 <stdlib.h>'s too (7.24p2): both
 * descriptors realize this one body.
 *
 * Windows has no call_once; its one-time initialization is kernel32's InitOnceExecuteOnce, whose callback takes
 * (INIT_ONCE *, parameter, context *) and returns BOOL, where C's takes nothing and returns nothing. So this unit
 * passes InitOnceExecuteOnce an ADAPTER and the program's function as the adapter's parameter — boxed, so a function
 * pointer never travels through an object pointer. once_flag on pe IS an INIT_ONCE (one pointer, zero-initialized:
 * ONCE_FLAG_INIT `{0}`), so the flag goes to the kernel as it is. The body used to be synthesized into every program
 * that included <threads.h>, called or not; as a runtime unit it is linked only into a program that calls call_once.
 *
 * pe's only: glibc exports call_once (ELF imports it), and on Mach-O call_once IS libSystem's pthread_once under
 * another name (the row's `linkName`) — once_flag there is a pthread_once_t and ONCE_FLAG_INIT its initializer. */
#include <stddef.h>
#include <threads.h>

struct dss_once_box {
    void (*func)(void);
};

static int dss_once_adapter(void *initOnce, void *parameter, void **context) {
    (void)initOnce;
    (void)context;
    ((struct dss_once_box *)parameter)->func();
    return 1;   /* TRUE: the initialization succeeded, so the flag is marked done */
}

void call_once(once_flag *flag, void (*func)(void)) {
    struct dss_once_box box = {func};
    (void)__dss_platform_init_once_execute_once(flag, dss_once_adapter, &box, NULL);
}
