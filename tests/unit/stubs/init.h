/* Test stub for init.h: the INIT_FUNC constructor registration is a no-op
 * so units can be compiled standalone. */
#ifndef _TEST_STUB_INIT_H_
#define _TEST_STUB_INIT_H_

typedef int (*InitFunc)(void);

typedef enum
{
    INIT_FUNC_EARLY = 0,
    INIT_FUNC_MIDDLE,
    INIT_FUNC_END,
} InitFuncPriority;

#define INIT_FUNC(priority, func) \
    static InitFunc _test_unused_##func __attribute__((unused)) = (func)

#define COMMON_INIT_NAME "common"

#endif
