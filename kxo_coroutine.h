#ifndef KXO_COROUTINE_H
#define KXO_COROUTINE_H

#include <linux/ktime.h>

struct kxo_coroutine {
    void (*func)(struct kxo_coroutine *);
    int state;
    ktime_t start;
    ktime_t end;
    char role;  // 'O' or 'X'
};

#define CORO_BEGIN(c)     \
    switch ((c)->state) { \
    case 0:
#define CORO_YIELD(c)          \
    do {                       \
        (c)->state = __LINE__; \
        return;                \
    case __LINE__:;            \
    } while (0)
#define CORO_END(c) \
    }               \
    (c)->state = 0

#endif  // KXO_COROUTINE_H
