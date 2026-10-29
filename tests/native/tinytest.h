// tinytest.h - a tiny, dependency-free C test framework for Windows (MinGW-w64, native or under Wine).
//
//   #define TT_IMPLEMENTATION      // in exactly one .c file, before including this header
//   #include "tinytest.h"
//
//   TEST(name) { CHECK(1 + 1 == 2); REQUIRE_EQ_INT(f(), 3); }
//   KNOWN_BUG_TEST(name, "src/file.c:123: what is wrong") { ...asserts the fixed behaviour... }
//
// CHECK_* records a failure and continues; REQUIRE_* records it and ends the test. Only CHECK_*
// may be used from worker threads. SKIP("why") ends the test as skipped. tt_defer(fn, arg)
// registers a cleanup that runs after the test even when it fails (LIFO).
//
// Known-bug tests document a bug that is not fixed yet: they are listed but not run unless
// --known-bugs is passed or BD3D_RUN_KNOWN_BUGS=1 is set. Once the bug is fixed, turn them into
// TEST() so they guard against regressions.
//
// Command line: [--list] [--known-bugs] [--verbose] [substring filters...]. Other "--x" options are
// left for the test program (tt_option() reads them). Exit code: 0 all passed, 1 a test failed,
// 2 the test program crashed or hung.

#ifndef TINYTEST_H
#define TINYTEST_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

typedef void (*tt_fn)(void);

void tt_register(const char *name, tt_fn fn, int line, const char *known_bug);
void tt_fail(const char *file, int line, int fatal, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void tt_skip(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void tt_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void tt_defer(void (*fn)(void *), void *arg);
const char *tt_option(const char *name); // value of "--name=value" or "--name value", or NULL
void tt_set_args(int argc, char **argv);  // makes tt_option() work before tt_main()
int tt_main(int argc, char **argv);
extern int tt_verbose;

#define TT_CAT2(a, b) a##b
#define TT_CAT(a, b) TT_CAT2(a, b)

#define TEST(name)                                                                                   \
    static void name(void);                                                                          \
    __attribute__((constructor)) static void TT_CAT(tt_reg_, name)(void)                             \
    {                                                                                                \
        tt_register(#name, name, __LINE__, NULL);                                                    \
    }                                                                                                \
    static void name(void)

#define KNOWN_BUG_TEST(name, why)                                                                    \
    static void name(void);                                                                          \
    __attribute__((constructor)) static void TT_CAT(tt_reg_, name)(void)                             \
    {                                                                                                \
        tt_register(#name, name, __LINE__, why);                                                     \
    }                                                                                                \
    static void name(void)

#define TT_CHECK_IMPL(fatal, cond, ...)                                                              \
    do {                                                                                             \
        if (!(cond)) tt_fail(__FILE__, __LINE__, fatal, __VA_ARGS__);                                \
    } while (0)

#define CHECK(cond) TT_CHECK_IMPL(0, cond, "CHECK(%s)", #cond)
#define REQUIRE(cond) TT_CHECK_IMPL(1, cond, "REQUIRE(%s)", #cond)
#define CHECK_MSG(cond, ...) TT_CHECK_IMPL(0, cond, __VA_ARGS__)
#define REQUIRE_MSG(cond, ...) TT_CHECK_IMPL(1, cond, __VA_ARGS__)

#define TT_EQ_INT(fatal, a, b)                                                                       \
    do {                                                                                             \
        long long tt_a = (long long)(a), tt_b = (long long)(b);                                      \
        if (tt_a != tt_b)                                                                            \
            tt_fail(__FILE__, __LINE__, fatal, "%s == %s: got %lld (0x%llx), expected %lld (0x%llx)", \
                    #a, #b, tt_a, (unsigned long long)tt_a, tt_b, (unsigned long long)tt_b);         \
    } while (0)
#define CHECK_EQ_INT(a, b) TT_EQ_INT(0, a, b)
#define REQUIRE_EQ_INT(a, b) TT_EQ_INT(1, a, b)

#define TT_EQ_PTR(fatal, a, b)                                                                       \
    do {                                                                                             \
        const void *tt_a = (const void *)(a), *tt_b = (const void *)(b);                             \
        if (tt_a != tt_b)                                                                            \
            tt_fail(__FILE__, __LINE__, fatal, "%s == %s: got %p, expected %p", #a, #b, tt_a, tt_b);  \
    } while (0)
#define CHECK_EQ_PTR(a, b) TT_EQ_PTR(0, a, b)
#define REQUIRE_EQ_PTR(a, b) TT_EQ_PTR(1, a, b)

#define TT_EQ_STR(fatal, a, b)                                                                       \
    do {                                                                                             \
        const char *tt_a = (a), *tt_b = (b);                                                         \
        if (!tt_a || !tt_b || strcmp(tt_a, tt_b) != 0)                                               \
            tt_fail(__FILE__, __LINE__, fatal, "%s == %s: got \"%s\", expected \"%s\"", #a, #b,       \
                    tt_a ? tt_a : "(null)", tt_b ? tt_b : "(null)");                                 \
    } while (0)
#define CHECK_EQ_STR(a, b) TT_EQ_STR(0, a, b)
#define REQUIRE_EQ_STR(a, b) TT_EQ_STR(1, a, b)

#define TT_EQ_WSTR(fatal, a, b)                                                                      \
    do {                                                                                             \
        const wchar_t *tt_a = (a), *tt_b = (b);                                                      \
        if (!tt_a || !tt_b || wcscmp(tt_a, tt_b) != 0)                                               \
            tt_fail(__FILE__, __LINE__, fatal, "%s == %s: got L\"%ls\", expected L\"%ls\"", #a, #b,   \
                    tt_a ? tt_a : L"(null)", tt_b ? tt_b : L"(null)");                               \
    } while (0)
#define CHECK_EQ_WSTR(a, b) TT_EQ_WSTR(0, a, b)
#define REQUIRE_EQ_WSTR(a, b) TT_EQ_WSTR(1, a, b)

#define TT_NEAR(fatal, a, b, eps)                                                                    \
    do {                                                                                             \
        double tt_a = (double)(a), tt_b = (double)(b);                                               \
        if (!(fabs(tt_a - tt_b) <= (eps)))                                                           \
            tt_fail(__FILE__, __LINE__, fatal, "%s ~= %s: got %.6g, expected %.6g", #a, #b, tt_a, tt_b); \
    } while (0)
#define CHECK_NEAR(a, b, eps) TT_NEAR(0, a, b, eps)
#define REQUIRE_NEAR(a, b, eps) TT_NEAR(1, a, b, eps)

#define SKIP(...) tt_skip(__VA_ARGS__)

#endif // TINYTEST_H

// ===================================================================================== implementation
#ifdef TT_IMPLEMENTATION
#undef TT_IMPLEMENTATION

#define TT_MAX_TESTS 512
#define TT_MAX_DEFER 256

typedef struct {
    const char *name;
    tt_fn fn;
    int line;
    const char *known_bug;
} tt_test;

static tt_test tt_tests[TT_MAX_TESTS];
static int tt_ntests;
static CRITICAL_SECTION tt_lock;
static volatile LONG tt_cur_failures;
static int tt_cur_skipped;
static char tt_skip_reason[512];
static intptr_t tt_jmp[5];
static DWORD tt_main_thread;
static const char *tt_cur_name;
static volatile LONG tt_cur_seq;
static int tt_argc;
static char **tt_argv;
static struct { void (*fn)(void *); void *arg; } tt_defers[TT_MAX_DEFER];
static int tt_ndefers;
static int tt_npassed, tt_nfailed, tt_nskipped, tt_nknown;
int tt_verbose;
static DWORD tt_timeout_ms = 180000;

void tt_register(const char *name, tt_fn fn, int line, const char *known_bug)
{
    if (tt_ntests >= TT_MAX_TESTS) {
        fprintf(stderr, "tinytest: too many tests\n");
        ExitProcess(2);
    }
    tt_tests[tt_ntests].name = name;
    tt_tests[tt_ntests].fn = fn;
    tt_tests[tt_ntests].line = line;
    tt_tests[tt_ntests].known_bug = known_bug;
    tt_ntests++;
}

static void tt_vprint(const char *prefix, const char *fmt, va_list ap)
{
    EnterCriticalSection(&tt_lock);
    fputs(prefix, stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    LeaveCriticalSection(&tt_lock);
}

void tt_fail(const char *file, int line, int fatal, const char *fmt, ...)
{
    const char *base = file;
    for (const char *p = file; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    InterlockedIncrement(&tt_cur_failures);
    char prefix[300];
    snprintf(prefix, sizeof(prefix), "    %s:%d: ", base, line);
    va_list ap;
    va_start(ap, fmt);
    tt_vprint(prefix, fmt, ap);
    va_end(ap);
    if (fatal) {
        if (GetCurrentThreadId() != tt_main_thread) {
            fprintf(stdout, "    (REQUIRE used on a worker thread: ending the thread)\n");
            ExitThread(1);
        }
        __builtin_longjmp(tt_jmp, 1);
    }
}

void tt_skip(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tt_skip_reason, sizeof(tt_skip_reason), fmt, ap);
    va_end(ap);
    tt_cur_skipped = 1;
    __builtin_longjmp(tt_jmp, 1);
}

void tt_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    tt_vprint("    note: ", fmt, ap);
    va_end(ap);
}

void tt_defer(void (*fn)(void *), void *arg)
{
    if (tt_ndefers >= TT_MAX_DEFER) {
        // Running fn now would free what the test is still using, so leak it and fail instead.
        tt_fail(__FILE__, __LINE__, 0, "more than %d tt_defer calls in one test; raise TT_MAX_DEFER", TT_MAX_DEFER);
        return;
    }
    tt_defers[tt_ndefers].fn = fn;
    tt_defers[tt_ndefers].arg = arg;
    tt_ndefers++;
}

void tt_set_args(int argc, char **argv)
{
    tt_argc = argc;
    tt_argv = argv;
}

const char *tt_option(const char *name)
{
    size_t n = strlen(name);
    for (int i = 1; i < tt_argc; i++) {
        const char *a = tt_argv[i];
        if (a[0] != '-' || a[1] != '-' || strncmp(a + 2, name, n) != 0) continue;
        if (a[2 + n] == '=') return a + 3 + n;
        if (a[2 + n] == 0 && i + 1 < tt_argc) return tt_argv[i + 1];
    }
    return NULL;
}

static void tt_print_summary(void)
{
    printf("\n==== %d passed, %d failed, %d skipped, %d known-bug tests not run (of %d)\n", tt_npassed,
           tt_nfailed, tt_nskipped, tt_nknown, tt_ntests);
    fflush(stdout);
}

static LONG WINAPI tt_crash_filter(EXCEPTION_POINTERS *ep)
{
    printf("[ CRASHED  ] %s: exception 0x%08lx at %p\n", tt_cur_name ? tt_cur_name : "(outside a test)",
           ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    tt_nfailed++;
    tt_print_summary();
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 2);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI tt_watchdog(LPVOID arg)
{
    for (;;) {
        LONG seq = tt_cur_seq;
        if (WaitForSingleObject((HANDLE)arg, tt_timeout_ms) == WAIT_OBJECT_0) return 0;
        if (seq == tt_cur_seq && tt_cur_name) {
            printf("[  HUNG    ] %s: no progress for %lu s (deadlock?)\n", tt_cur_name, tt_timeout_ms / 1000);
            tt_nfailed++;
            tt_print_summary();
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 2);
        }
    }
}

static int tt_cmp(const void *a, const void *b)
{
    const tt_test *x = a, *y = b;
    return x->line - y->line;
}

static int tt_is_option_with_value(const char *a)
{
    // Options of the test programs that take a separate value.
    return !strcmp(a, "--dll") || !strcmp(a, "--plugin") || !strcmp(a, "--timeout");
}

int tt_main(int argc, char **argv)
{
    tt_argc = argc;
    tt_argv = argv;
    InitializeCriticalSection(&tt_lock);
    tt_main_thread = GetCurrentThreadId();
    setvbuf(stdout, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(tt_crash_filter);
    qsort(tt_tests, tt_ntests, sizeof(tt_tests[0]), tt_cmp);

    int list = 0, known = 0;
    const char *env = getenv("BD3D_RUN_KNOWN_BUGS");
    if (env && env[0] && strcmp(env, "0") != 0) known = 1;
    const char *filters[64];
    int nfilters = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--known-bugs")) known = 1;
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) tt_verbose = 1;
        else if (tt_is_option_with_value(argv[i])) i++;
        else if (argv[i][0] == '-' && argv[i][1] == '-') continue;
        else if (nfilters < 64) filters[nfilters++] = argv[i];
    }
    const char *to = tt_option("timeout");
    if (to) tt_timeout_ms = (DWORD)atoi(to) * 1000;

    if (list) {
        for (int i = 0; i < tt_ntests; i++)
            printf("%s%s\n", tt_tests[i].name, tt_tests[i].known_bug ? "  (known bug)" : "");
        return 0;
    }

    HANDLE stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE wd = CreateThread(NULL, 0, tt_watchdog, stop, 0, NULL);

    for (int i = 0; i < tt_ntests; i++) {
        tt_test *t = &tt_tests[i];
        if (nfilters) {
            int match = 0;
            for (int f = 0; f < nfilters; f++)
                if (strstr(t->name, filters[f])) match = 1;
            if (!match) continue;
        }
        if (t->known_bug && !known) {
            printf("[ KNOWNBUG ] %s: not run (%s). Run with --known-bugs to check a fix.\n", t->name, t->known_bug);
            tt_nknown++;
            continue;
        }
        printf("[ RUN      ] %s%s\n", t->name, t->known_bug ? "  (known bug)" : "");
        tt_cur_failures = 0;
        tt_cur_skipped = 0;
        tt_ndefers = 0;
        tt_cur_name = t->name;
        InterlockedIncrement(&tt_cur_seq);
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        if (__builtin_setjmp(tt_jmp) == 0) {
            t->fn();
        }
        while (tt_ndefers > 0) {
            tt_ndefers--;
            tt_defers[tt_ndefers].fn(tt_defers[tt_ndefers].arg);
        }
        QueryPerformanceCounter(&t1);
        long ms = (long)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart);
        if (tt_cur_failures) {
            printf("[  FAILED  ] %s (%ld ms)%s\n", t->name, ms, t->known_bug ? " - known bug, still present" : "");
            tt_nfailed++;
        } else if (tt_cur_skipped) {
            printf("[ SKIPPED  ] %s: %s\n", t->name, tt_skip_reason);
            tt_nskipped++;
        } else {
            printf("[       OK ] %s (%ld ms)%s\n", t->name, ms, t->known_bug ? " - known bug looks FIXED: make it a TEST()" : "");
            tt_npassed++;
        }
        tt_cur_name = NULL;
    }
    SetEvent(stop);
    WaitForSingleObject(wd, 5000);
    tt_print_summary();
    return tt_nfailed ? 1 : 0;
}

#endif // TT_IMPLEMENTATION
