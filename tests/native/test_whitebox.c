// White-box tests: this file #includes src/braindance3d_sdk.c so it can call its static functions and
// read its static state (log budgets, settings, install-folder discovery, log rotation, monitor cache).
// It builds into an .exe; DllMain is never called, each test sets up the state it needs.
//
// Run: test_whitebox.exe [--dll path\to\Odyssey3DSDK.dll] [--list] [name filters...]

#include "../../src/braindance3d_sdk.c"

#define TT_IMPLEMENTATION
#include "tinytest.h"
#include "testutil.h"

// ===================================================================================== state reset

static void wb_close_log(void *unused)
{
    if (g_log) fclose(g_log);
    g_log = NULL;
}

// Puts the DLL's globals back to their load-time defaults.
static void wb_begin(void)
{
    static int once;
    if (!once) {
        InitializeCriticalSection(&g_logLock);
        InitializeCriticalSection(&g_monLock);
        install_known_slots();
        once = 1;
    }
    wb_close_log(NULL);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_logLevel = 1;
    g_monitorPrimaryOnly = 0;
    g_gameWnd = NULL;
    g_windowSearchTime = -1.0;
    g_fullLogCalls = 10;
    g_summaryInterval = 60.0;
    g_installDir[0] = 0;
    g_logPath[0] = 0;
    memset(g_facStats, 0, sizeof(g_facStats));
    memset(g_devStats, 0, sizeof(g_devStats));
    memset(g_wvStats, 0, sizeof(g_wvStats));
    g_monitorTime = -1.0;
    g_lastMonitor = NULL;
    SetRectEmpty(&g_lastLoggedRect);
    tt_defer(wb_close_log, NULL);
}

// Opens a log file in a temp folder as the DLL would, so should_log/slog are live.
static wchar_t *wb_open_log(void)
{
    wchar_t *dir = tu_tempdir(L"wb");
    g_log = open_log_in(dir);
    REQUIRE(g_log != NULL);
    return dir;
}

static long long wb_log_size(void)
{
    fflush(g_log);
    return tu_file_size(g_logPath);
}

static void wb_free(void *p) { free(p); }

static int wb_log_lines(void)
{
    fflush(g_log);
    char *t = tu_read_file(g_logPath, NULL);
    if (!t) return -1;
    int n = tu_count_lines(t);
    free(t);
    return n;
}

// Sets g_installDir to dir and writes dir\Braindance3D-Settings.ini (when text != NULL), then reads settings.
static void wb_settings(const wchar_t *dir, const char *text)
{
    wcscpy(g_installDir, dir);
    wchar_t ini[TU_PATH];
    tu_join(ini, TU_PATH, dir, SETTINGS_FILE);
    DeleteFileW(ini);
    if (text) REQUIRE(tu_write_text(ini, text));
    read_settings();
}

// ===================================================================================== path helpers

TEST(strip_last_removes_the_last_component)
{
    wchar_t p[64];
    wcscpy(p, L"C:\\a\\b\\c.dll");
    CHECK_EQ_INT(strip_last(p), 1);
    CHECK_EQ_WSTR(p, L"C:\\a\\b");
    CHECK_EQ_INT(strip_last(p), 1);
    CHECK_EQ_WSTR(p, L"C:\\a");
    CHECK_EQ_INT(strip_last(p), 1);
    CHECK_EQ_WSTR(p, L"C:");
    CHECK_EQ_INT(strip_last(p), 0);
    CHECK_EQ_WSTR(p, L"C:");
    wcscpy(p, L"dir\\");
    CHECK_EQ_INT(strip_last(p), 1);
    CHECK_EQ_WSTR(p, L"dir");
    wcscpy(p, L"");
    CHECK_EQ_INT(strip_last(p), 0);
}

TEST(utf8_converts_and_handles_small_buffers)
{
    char buf[64];
    CHECK_EQ_STR(utf8(L"abc", buf, sizeof(buf)), "abc");
    CHECK_EQ_PTR(utf8(L"abc", buf, sizeof(buf)), buf);
    CHECK_EQ_STR(utf8(L"\u00e4\u2603", buf, sizeof(buf)), "\xc3\xa4\xe2\x98\x83");
    CHECK_EQ_STR(utf8(L"", buf, sizeof(buf)), "");
    char tiny[3];
    CHECK_EQ_STR(utf8(L"\u2603", tiny, sizeof(tiny)), ""); // doesn't fit: empty, not truncated garbage
}

static HMODULE wb_load_copy(const wchar_t *root)
{
    wchar_t core[TU_PATH], dll[TU_PATH], ini[TU_PATH];
    tu_join(core, TU_PATH, root, L"GameLauncherData\\core");
    tu_join(dll, TU_PATH, core, L"Odyssey3DSDK.dll");
    tu_join(ini, TU_PATH, root, SETTINGS_FILE);
    tu_require_dll();
    REQUIRE(tu_mkdirs(core));
    REQUIRE(CopyFileW(tu_dll_path(), dll, FALSE));
    REQUIRE(tu_write_text(ini, "[Log]\nLevel=0\n")); // the copy's own DllMain stays quiet
    HMODULE h = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    REQUIRE_MSG(h != NULL, "LoadLibraryEx failed (%lu)", GetLastError());
    return h;
}

static void wb_free_library(void *h) { FreeLibrary((HMODULE)h); }

TEST(find_install_dir_is_two_levels_above_the_dll)
{
    wb_begin();
    wchar_t *tmp = tu_tempdir(L"find");
    wchar_t root[TU_PATH];
    tu_join(root, TU_PATH, tmp, L"Cyberpunk 2077\\bin\\x64\\Braindance3D");
    HMODULE h = wb_load_copy(root);
    tt_defer(wb_free_library, h);
    HMODULE saved = g_module;
    g_module = h;
    find_install_dir();
    g_module = saved;
    CHECK_EQ_WSTR(g_installDir, root);
}

TEST(find_install_dir_with_non_ascii_path)
{
    wb_begin();
    wchar_t *tmp = tu_tempdir(L"find-u");
    wchar_t root[TU_PATH];
    tu_join(root, TU_PATH, tmp, TU_NONASCII L" [GOG] (x86)");
    HMODULE h = wb_load_copy(root);
    tt_defer(wb_free_library, h);
    HMODULE saved = g_module;
    g_module = h;
    find_install_dir();
    g_module = saved;
    CHECK_EQ_WSTR(g_installDir, root);
}

// ===================================================================================== settings

TEST(read_settings_level_and_budgets)
{
    wb_begin();
    wchar_t *dir = tu_tempdir(L"ini");
    struct { const char *ini; int level; LONG64 full; double interval; } cases[] = {
        { NULL, 1, 10, 60.0 },                                // no file: default
        { "", 1, 10, 60.0 },                                  // empty file
        { "[Log]\n", 1, 10, 60.0 },                           // no key
        { "[Other]\nLevel=0\n", 1, 10, 60.0 },                // wrong section
        { "[Log]\nLevel=0\n", 0, 10, 60.0 },
        { "[Log]\nLevel=1\n", 1, 10, 60.0 },
        { "[Log]\nLevel=2\n", 2, 120, 1.0 },
        { "[Log]\r\nLevel = 2\r\n", 2, 120, 1.0 },            // CRLF and spaces
        { "[log]\nlevel=0\n", 0, 10, 60.0 },                  // case-insensitive
        { "; c\n[Log]\n; 0 = off\nLevel = 1\n", 1, 10, 60.0 }, // comments
        { "[Log]\nLevel=3\n", 2, 120, 1.0 },                  // clamped
        { "[Log]\nLevel=99\n", 2, 120, 1.0 },
        { "[Log]\nLevel=7\n", 2, 120, 1.0 },
        { "[Log]\nLevel=02\n", 2, 120, 1.0 },
        { "[Log]\nLevel=2 ; verbose\n", 2, 120, 1.0 },
        { "[Log]\nLevel=00\n", 0, 10, 60.0 },
        { "[Log]\nLevel=-1\n", 1, 10, 60.0 },               // not a digit: default
        { "[Log]\nLevel=-100\n", 1, 10, 60.0 },
        { "[Log]\nLevel=abc\n", 1, 10, 60.0 },
        { "[Log]\nLevel=off\n", 1, 10, 60.0 },
        { "[Log]\nLevel=\n", 1, 10, 60.0 },
        { "[Log]\nLevel=99999999999999999999\n", 2, 120, 1.0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g_logLevel = 42;
        g_fullLogCalls = 4242;
        g_summaryInterval = 42.0;
        wb_settings(dir, cases[i].ini);
        CHECK_MSG(g_logLevel == cases[i].level, "case %u (%s): level %d, expected %d", (unsigned)i,
                  cases[i].ini ? cases[i].ini : "no file", g_logLevel, cases[i].level);
        CHECK_MSG(g_fullLogCalls == cases[i].full, "case %u: full-log budget %lld, expected %lld", (unsigned)i,
                  (long long)g_fullLogCalls, (long long)cases[i].full);
        CHECK_MSG(g_summaryInterval == cases[i].interval, "case %u: summary interval %g, expected %g", (unsigned)i,
                  g_summaryInterval, cases[i].interval);
    }
}

TEST(read_settings_from_non_ascii_folder)
{
    wb_begin();
    wchar_t *tmp = tu_tempdir(L"ini-u"), dir[TU_PATH];
    tu_join(dir, TU_PATH, tmp, TU_NONASCII);
    REQUIRE(tu_mkdirs(dir));
    wb_settings(dir, "[Log]\nLevel=2\n");
    CHECK_EQ_INT(g_logLevel, 2);
}

TEST(read_settings_with_install_dir_at_max_path_does_not_overflow)
{
    wb_begin();
    for (int i = 0; i < MAX_PATH - 1; i++) g_installDir[i] = L'a';
    g_installDir[MAX_PATH - 1] = 0;
    read_settings(); // must not write past ini[MAX_PATH]
    CHECK_EQ_INT(g_logLevel, 1);
}

TEST(parse_level_values)
{
    struct { const wchar_t *v; int level; } cases[] = {
        { L"0", 0 }, { L"1", 1 }, { L"2", 2 }, { L"3", 2 }, { L"9", 2 }, { L"  2", 2 }, { L"\t0", 0 },
        { L"2abc", 2 }, { L"0x2", 0 }, { L"", 1 }, { L"-1", 1 }, { L"+2", 1 }, { L"abc", 1 }, { L"verbose", 1 },
        { L"99999999999999999999", 2 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        CHECK_MSG(parse_level(cases[i].v) == cases[i].level, "parse_level(\"%ls\") = %d, expected %d", cases[i].v,
                  parse_level(cases[i].v), cases[i].level);
}

TEST(read_settings_display_monitor)
{
    wb_begin();
    wchar_t *dir = tu_tempdir(L"mon");
    struct { const char *ini; int primary; } cases[] = {
        { NULL, 0 },
        { "[Display]\nMonitor=auto\n", 0 },
        { "[Display]\nMonitor=primary\n", 1 },
        { "[Display]\r\nMonitor = Primary\r\n", 1 },
        { "[display]\nmonitor=PRIMARY\n", 1 },
        { "[Display]\nMonitor=\n", 0 },
        { "[Display]\nMonitor=secondary\n", 0 },
        { "[Log]\nMonitor=primary\n", 0 }, // wrong section
        { "[Display]\nMonitor=1\n", 0 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g_monitorPrimaryOnly = 42;
        wb_settings(dir, cases[i].ini);
        CHECK_MSG(g_monitorPrimaryOnly == cases[i].primary, "case %u (%s): primary=%d", (unsigned)i,
                  cases[i].ini ? cases[i].ini : "no file", g_monitorPrimaryOnly);
    }
}

TEST(is_allowed_host_needs_cyberpunk2077_exe_or_the_override)
{
    // This test program is not Cyberpunk2077.exe.
    tu_setenv(L"BRAINDANCE3D_ANY_EXE", NULL);
    CHECK_EQ_INT(is_allowed_host(), 0);
    const wchar_t *no[] = { L"0", L"", L"true", L"11", L"1 ", L"yes" };
    for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++) {
        SetEnvironmentVariableW(L"BRAINDANCE3D_ANY_EXE", no[i]);
        CHECK_MSG(is_allowed_host() == 0, "allowed with BRAINDANCE3D_ANY_EXE=\"%ls\"", no[i]);
    }
    SetEnvironmentVariableW(L"BRAINDANCE3D_ANY_EXE", L"1");
    CHECK_EQ_INT(is_allowed_host(), 1);
}

TEST(factory_export_follows_the_host_check)
{
    wb_begin();
    int saved = g_hostAllowed;
    g_hostAllowed = 0;
    CHECK(create_odyssey3d_factory_instance() == NULL);
    g_hostAllowed = 1;
    CHECK_EQ_PTR(create_odyssey3d_factory_instance(), &g_factory);
    g_hostAllowed = saved;
    CHECK_EQ_STR(braindance3d_version(), BD3D_VERSION_STR);
}

// ===================================================================================== should_log

static void check_budget(int level, LONG64 expect_full)
{
    wb_begin();
    wchar_t *dir = wb_open_log();
    char ini[64];
    snprintf(ini, sizeof(ini), "[Log]\nLevel=%d\n", level);
    wb_settings(dir, ini);
    SlotStats s = { 0 };
    int full = 0, last_full = 0;
    for (int i = 1; i <= 1000; i++)
        if (should_log(&s, "test")) {
            full++;
            last_full = i;
        }
    CHECK_EQ_INT(full, expect_full);
    CHECK_EQ_INT(last_full, expect_full); // the first N calls, not any N
    CHECK_EQ_INT(s.calls, 1000);
}

TEST(should_log_budget_level_1_is_10_calls) { check_budget(1, 10); }
TEST(should_log_budget_level_2_is_120_calls) { check_budget(2, 120); }

TEST(should_log_without_a_log_file_counts_but_never_logs)
{
    wb_begin();
    SlotStats s = { 0 };
    for (int i = 0; i < 100; i++) CHECK(!should_log(&s, "x"));
    CHECK_EQ_INT(s.calls, 100);
}

TEST(should_log_writes_a_summary_once_per_interval)
{
    wb_begin();
    wb_open_log();
    SlotStats s = { 0 };
    for (int i = 0; i < 10; i++) should_log(&s, "x");
    long long size0 = wb_log_size();
    // Within the interval: no output.
    for (int i = 0; i < 1000; i++) CHECK(!should_log(&s, "x"));
    CHECK_EQ_INT(wb_log_size(), size0);
    // Interval elapsed: exactly one summary line, then quiet again.
    int lines0 = wb_log_lines();
    s.lastSummaryTime = now_seconds() - g_summaryInterval - 0.5;
    CHECK(!should_log(&s, "x"));
    CHECK_EQ_INT(wb_log_lines(), lines0 + 1);
    CHECK_EQ_INT(s.lastSummaryCalls, s.calls);
    for (int i = 0; i < 1000; i++) should_log(&s, "x");
    CHECK_EQ_INT(wb_log_lines(), lines0 + 1);
}

typedef struct { SlotStats *s; volatile LONG *full; int n; } SlArgs;

static DWORD WINAPI sl_thread(LPVOID p)
{
    SlArgs *a = p;
    for (int i = 0; i < a->n; i++)
        if (should_log(a->s, "mt")) InterlockedIncrement(a->full);
    return 0;
}

TEST(should_log_is_exact_under_contention)
{
    wb_begin();
    wchar_t *dir = wb_open_log();
    wb_settings(dir, "[Log]\nLevel=2\n");
    g_summaryInterval = 0.001; // also exercise the summary path from all threads
    SlotStats s = { 0 };
    volatile LONG full = 0;
    SlArgs a = { &s, &full, 50000 };
    HANDLE th[8];
    for (int i = 0; i < 8; i++) th[i] = CreateThread(NULL, 0, sl_thread, &a, 0, NULL);
    REQUIRE(WaitForMultipleObjects(8, th, TRUE, 120000) < WAIT_OBJECT_0 + 8);
    for (int i = 0; i < 8; i++) CloseHandle(th[i]);
    CHECK_EQ_INT(s.calls, 8 * 50000);
    CHECK_EQ_INT(full, 120);
    CHECK(s.lastSummaryCalls <= s.calls);
}

TEST(each_known_slot_counts_its_own_calls)
{
    wb_begin();
    Device dev = { g_devVtbl };
    float f3[3];
    int64_t r4[4];
    ((float *(*)(Device *, float *, int))g_devVtbl[1])(&dev, f3, 0);
    ((float *(*)(Device *, float *))g_devVtbl[2])(&dev, f3);
    ((float *(*)(Device *, float *))g_devVtbl[3])(&dev, f3);
    ((int64_t * (*)(Device *, int64_t *)) g_devVtbl[6])(&dev, r4);
    ((uintptr_t(*)(Device *, uintptr_t))g_devVtbl[9])(&dev, 0);
    ((unsigned char (*)(Device *))g_devVtbl[10])(&dev);
    ((uintptr_t(*)(Device *, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))g_devVtbl[5])(&dev, 1, 2, 3, 4, 5);
    for (int i = 0; i < NSLOTS; i++) {
        int expect = (i == 1 || i == 2 || i == 3 || i == 6 || i == 9 || i == 10 || i == 5) ? 1 : 0;
        CHECK_MSG(g_devStats[i].calls == expect, "device slot %d: %lld calls", i, (long long)g_devStats[i].calls);
    }
    Weaver wv = { g_wvVtbl };
    ((void (*)(Weaver *, unsigned, unsigned, int, int))g_wvVtbl[2])(&wv, 1, 1, 0, 0);
    ((void (*)(Weaver *, ID3D12Resource *, int))g_wvVtbl[4])(&wv, NULL, 0);
    ((void (*)(Weaver *, ID3D12Resource *))g_wvVtbl[6])(&wv, NULL);
    ((void (*)(Weaver *, ID3D12GraphicsCommandList *))g_wvVtbl[7])(&wv, NULL);
    CHECK_EQ_INT(g_wvStats[2].calls, 1);
    CHECK_EQ_INT(g_wvStats[4].calls, 1);
    CHECK_EQ_INT(g_wvStats[6].calls, 1);
    CHECK_EQ_INT(g_wvStats[7].calls, 1);
}

// ===================================================================================== vtables

TEST(known_slots_match_the_documented_table)
{
    wb_begin();
    CHECK_EQ_PTR(g_facVtbl[0], fac_CreateDevice);
    CHECK_EQ_PTR(g_facVtbl[2], fac_CreateWeaver);
    CHECK_EQ_PTR(g_devVtbl[1], dev_GetEyePosition);
    CHECK_EQ_PTR(g_devVtbl[2], dev_GetResolution);
    CHECK_EQ_PTR(g_devVtbl[3], dev_GetFloat2_18);
    CHECK_EQ_PTR(g_devVtbl[6], dev_GetMonitorRect);
    CHECK_EQ_PTR(g_devVtbl[9], dev_Hint48);
    CHECK_EQ_PTR(g_devVtbl[10], dev_IsAvailable);
    CHECK_EQ_PTR(g_devVtbl[12], dev_Destroy);
    CHECK_EQ_PTR(g_wvVtbl[2], wv_Weave);
    CHECK_EQ_PTR(g_wvVtbl[4], wv_SetInputViewTexture);
    CHECK_EQ_PTR(g_wvVtbl[6], wv_SetOutputFrameBuffer);
    CHECK_EQ_PTR(g_wvVtbl[7], wv_SetCommandList);
    // Everything else is the generic stub of that index.
    void *facGen[NSLOTS] = SLOT_LIST(fac), *devGen[NSLOTS] = SLOT_LIST(dev), *wvGen[NSLOTS] = SLOT_LIST(wv);
    for (int i = 0; i < NSLOTS; i++) {
        if (i != 0 && i != 2) CHECK_MSG(g_facVtbl[i] == facGen[i], "factory slot %d", i);
        if (i != 1 && i != 2 && i != 3 && i != 6 && i != 9 && i != 10 && i != 12)
            CHECK_MSG(g_devVtbl[i] == devGen[i], "device slot %d", i);
        if (i != 2 && i != 4 && i != 6 && i != 7) CHECK_MSG(g_wvVtbl[i] == wvGen[i], "weaver slot %d", i);
    }
}

TEST(slot_names_for_the_call_totals)
{
    CHECK_EQ_STR(slot_name("device", 1), "GetEyePosition");
    CHECK_EQ_STR(slot_name("device", 10), "IsAvailable");
    CHECK_EQ_STR(slot_name("device", 12), "Destroy");
    CHECK_EQ_STR(slot_name("device", 5), "(unmapped)");
    CHECK_EQ_STR(slot_name("factory", 0), "CreateDevice");
    CHECK_EQ_STR(slot_name("factory", 2), "CreateWeaver");
    CHECK_EQ_STR(slot_name("factory", 31), "(unmapped)");
    CHECK_EQ_STR(slot_name("weaver", 7), "SetCommandList");
    CHECK_EQ_STR(slot_name("weaver", 0), "(unmapped)");
}

TEST(log_call_totals_lists_only_called_slots)
{
    wb_begin();
    wb_open_log();
    int lines0 = wb_log_lines();
    g_devStats[1].calls = 5;
    g_devStats[10].calls = 7;
    log_call_totals("device", g_devStats);
    CHECK_EQ_INT(wb_log_lines(), lines0 + 2);
    log_call_totals("weaver", g_wvStats);
    CHECK_EQ_INT(wb_log_lines(), lines0 + 2);
}

TEST(dxgi_format_names)
{
    CHECK_EQ_STR(dxgi_format_name(28), "R8G8B8A8_UNORM");
    CHECK_EQ_STR(dxgi_format_name(87), "B8G8R8A8_UNORM");
    CHECK_EQ_STR(dxgi_format_name(24), "R10G10B10A2_UNORM");
    CHECK_EQ_STR(dxgi_format_name(0), "?");
    CHECK_EQ_STR(dxgi_format_name(-1), "?");
    CHECK_EQ_STR(dxgi_format_name(100000), "?");
}

// ===================================================================================== log files

TEST(slog_without_a_log_is_a_no_op_and_with_one_writes_whole_lines)
{
    wb_begin();
    slog("nothing %d", 1);
    wb_open_log();
    int lines0 = wb_log_lines();
    slog("one %s", "line");
    slog("two");
    CHECK_EQ_INT(wb_log_lines(), lines0 + 2);
    fflush(g_log);
    char *t = tu_read_file(g_logPath, NULL);
    REQUIRE(t);
    tt_defer(wb_free, t);
    size_t n = strlen(t);
    CHECK(n > 0 && t[n - 1] == '\n');
    CHECK(strstr(t, "one line") != NULL);
}

TEST(open_log_in_rotates_the_previous_session)
{
    wb_begin();
    wchar_t *dir = tu_tempdir(L"rot"), log[TU_PATH], old[TU_PATH];
    tu_join(log, TU_PATH, dir, L"braindance3d.log");
    tu_join(old, TU_PATH, dir, L"braindance3d.old.log");
    REQUIRE(tu_write_text(log, "session-1\n"));
    REQUIRE(tu_write_text(old, "session-0\n"));
    g_log = open_log_in(dir);
    REQUIRE(g_log != NULL);
    CHECK_EQ_WSTR(g_logPath, log);
    CHECK_EQ_INT(tu_file_size(log), 0);
    char *t = tu_read_file(old, NULL);
    REQUIRE(t);
    tt_defer(wb_free, t);
    CHECK_EQ_STR(t, "session-1\n");
}

TEST(open_log_in_without_a_previous_log)
{
    wb_begin();
    wchar_t *dir = tu_tempdir(L"rot0"), old[TU_PATH];
    g_log = open_log_in(dir);
    REQUIRE(g_log != NULL);
    tu_join(old, TU_PATH, dir, L"braindance3d.old.log");
    CHECK(!tu_exists(old));
}

TEST(open_log_in_uses_a_pid_log_when_the_log_is_in_use)
{
    wb_begin();
    wchar_t *dir = tu_tempdir(L"busy"), log[TU_PATH], want[TU_PATH];
    tu_join(log, TU_PATH, dir, L"braindance3d.log");
    REQUIRE(tu_write_text(log, "busy\n"));
    HANDLE h = CreateFileW(log, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    g_log = open_log_in(dir);
    CloseHandle(h);
    REQUIRE(g_log != NULL);
    _snwprintf(want, TU_PATH, L"%ls\\braindance3d-%lu.log", dir, GetCurrentProcessId());
    CHECK_EQ_WSTR(g_logPath, want);
    CHECK_EQ_INT(tu_file_size(log), 5);
}

TEST(open_log_in_rejects_too_long_folders)
{
    wb_begin();
    wchar_t dir[MAX_PATH];
    wcscpy(dir, L"C:\\");
    while (wcslen(dir) < MAX_PATH - 10) wcscat(dir, L"x");
    CHECK(open_log_in(dir) == NULL);
    CHECK_EQ_WSTR(g_logPath, L"");
}

TEST(open_log_in_missing_folder_returns_null)
{
    wb_begin();
    wchar_t *tmp = tu_tempdir(L"missing"), dir[TU_PATH];
    tu_join(dir, TU_PATH, tmp, L"does-not-exist");
    CHECK(open_log_in(dir) == NULL);
}

TEST(log_open_level_0_opens_nothing)
{
    wb_begin();
    wchar_t *dir = tu_tempdir(L"l0");
    wb_settings(dir, "[Log]\nLevel=0\n");
    log_open();
    CHECK(g_log == NULL);
    wchar_t log[TU_PATH];
    tu_join(log, TU_PATH, dir, L"braindance3d.log");
    CHECK(!tu_exists(log));
    CHECK_EQ_WSTR(g_logPath, L"");
}

TEST(log_open_prefers_the_install_folder)
{
    wb_begin();
    wchar_t *dir = tu_tempdir(L"l1"), *lad = tu_tempdir(L"lad"), log[TU_PATH];
    tu_setenv(L"LOCALAPPDATA", lad);
    wb_settings(dir, NULL);
    log_open();
    REQUIRE(g_log != NULL);
    tu_join(log, TU_PATH, dir, L"braindance3d.log");
    CHECK_EQ_WSTR(g_logPath, log);
}

TEST(log_open_falls_back_to_localappdata)
{
    wb_begin();
    wchar_t *tmp = tu_tempdir(L"fb"), *lad = tu_tempdir(L"lad"), dir[TU_PATH], want[TU_PATH];
    tu_join(dir, TU_PATH, tmp, L"not-there"); // install folder missing = not writable
    tu_setenv(L"LOCALAPPDATA", lad);
    wcscpy(g_installDir, dir);
    log_open();
    REQUIRE(g_log != NULL);
    tu_join(want, TU_PATH, lad, L"Braindance3D\\braindance3d.log");
    CHECK_EQ_WSTR(g_logPath, want);
    CHECK(tu_is_dir(lad) && tu_exists(want));
}

TEST(log_open_with_no_usable_folder)
{
    wb_begin();
    wchar_t *tmp = tu_tempdir(L"none"), dir[TU_PATH];
    tu_join(dir, TU_PATH, tmp, L"not-there");
    wcscpy(g_installDir, dir);
    tu_setenv(L"LOCALAPPDATA", NULL);
    log_open();
    CHECK(g_log == NULL);
    // Logging calls are harmless without a file.
    slog("x");
    SlotStats s = { 0 };
    CHECK(!should_log(&s, "x"));
}

TEST(log_open_ignores_a_localappdata_that_is_too_long)
{
    wb_begin();
    wchar_t *tmp = tu_tempdir(L"long"), dir[TU_PATH], lad[MAX_PATH + 50];
    tu_join(dir, TU_PATH, tmp, L"not-there");
    wcscpy(g_installDir, dir);
    wcscpy(lad, tmp);
    while (wcslen(lad) < MAX_PATH - 30) wcscat(lad, L"\\x");
    tu_setenv(L"LOCALAPPDATA", lad);
    log_open();
    CHECK(g_log == NULL);
}

// ===================================================================================== monitor cache

TEST(current_monitor_is_cached_briefly_then_refreshed)
{
    wb_begin();
    RECT sentinel = { 1, 2, 3, 4 };
    g_monitorRect = sentinel;
    g_monitorTime = now_seconds();
    RECT r = current_monitor(0);
    CHECK(EqualRect(&r, &sentinel)); // GetResolution's path reuses a fresh-enough result
    g_monitorTime = now_seconds() - 2.0;
    r = current_monitor(0);
    POINT origin = {0, 0};
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    REQUIRE(GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi));
    CHECK(EqualRect(&r, &mi.rcMonitor));
}

TEST(current_monitor_fresh_always_rereads)
{
    wb_begin();
    RECT sentinel = { 1, 2, 3, 4 };
    g_monitorRect = sentinel;
    g_monitorTime = now_seconds();
    RECT r = current_monitor(1); // GetMonitorRect's path
    CHECK(!EqualRect(&r, &sentinel));
}

static HWND wb_window_ex(const wchar_t *title, DWORD ex, int x, int y, int w, int h)
{
    HWND hwnd = tu_window(title, x, y, w, h);
    if (ex) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | ex);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    return hwnd;
}

TEST(candidate_windows_exclude_tool_overlay_hidden_owned_and_empty_windows)
{
    wb_begin();
    DWORD pid = GetCurrentProcessId();
    HWND normal = wb_window_ex(L"game", 0, 10, 10, 300, 200);
    HWND tool = wb_window_ex(L"tool", WS_EX_TOOLWINDOW, 10, 10, 900, 700);
    HWND overlay = wb_window_ex(L"overlay", WS_EX_LAYERED | WS_EX_TRANSPARENT, 10, 10, 900, 700);
    HWND layered = wb_window_ex(L"layered only", WS_EX_LAYERED, 10, 10, 200, 100);
    HWND empty = wb_window_ex(L"empty", 0, 10, 10, 0, 0);
    HWND hidden = wb_window_ex(L"hidden", 0, 10, 10, 900, 700);
    ShowWindow(hidden, SW_HIDE);
    HWND owned = CreateWindowExW(0, L"bd3d-test-window", L"owned", WS_POPUP | WS_VISIBLE, 0, 0, 900, 700, normal, NULL,
                                 GetModuleHandleW(NULL), NULL);
    REQUIRE(owned != NULL);
    CHECK(is_candidate_window(normal, pid));
    CHECK(is_candidate_window(layered, pid)); // layered alone is a normal (e.g. fading) window
    CHECK(!is_candidate_window(tool, pid));
    CHECK(!is_candidate_window(overlay, pid));
    CHECK(!is_candidate_window(empty, pid));
    CHECK(!is_candidate_window(hidden, pid));
    CHECK(!is_candidate_window(owned, pid));
    CHECK(!is_candidate_window(normal, pid + 4)); // another process's
    HWND desktop = GetDesktopWindow();
    CHECK(!is_candidate_window(desktop, pid));
    // The search picks the largest remaining candidate.
    FindWindowCtx ctx = { pid, NULL, 0 };
    EnumWindows(find_game_window_cb, (LPARAM)&ctx);
    CHECK_EQ_PTR(ctx.best, normal);
    DestroyWindow(owned);
}

TEST(game_window_is_found_kept_and_replaced_when_it_disappears)
{
    wb_begin();
    HWND a = wb_window_ex(L"splash", 0, 10, 10, 300, 200);
    update_game_window(now_seconds());
    CHECK_EQ_PTR(g_gameWnd, a);
    // A bigger window appearing later doesn't steal it while the first one is still there.
    HWND b = wb_window_ex(L"main", 0, 10, 10, 800, 600);
    update_game_window(now_seconds() + 10);
    CHECK_EQ_PTR(g_gameWnd, a);
    // Once it is hidden, the next call finds the other one straight away.
    ShowWindow(a, SW_HIDE);
    update_game_window(now_seconds());
    CHECK_EQ_PTR(g_gameWnd, b);
    // Destroyed: no window at all.
    DestroyWindow(b);
    update_game_window(now_seconds());
    CHECK(g_gameWnd == NULL);
}

TEST(game_window_search_is_rate_limited_while_there_is_none)
{
    wb_begin();
    double t = now_seconds();
    update_game_window(t);
    CHECK(g_gameWnd == NULL);
    HWND w = wb_window_ex(L"late", 0, 10, 10, 300, 200);
    update_game_window(t + MONITOR_REFRESH_S / 2); // too soon: not searched again
    CHECK(g_gameWnd == NULL);
    update_game_window(t + MONITOR_REFRESH_S + 0.01);
    CHECK_EQ_PTR(g_gameWnd, w);
}

TEST(game_window_prefers_the_foreground_window)
{
    wb_begin();
    wb_window_ex(L"big", 0, 10, 10, 800, 600);
    HWND small = wb_window_ex(L"small", 0, 10, 10, 200, 100);
    SetForegroundWindow(small);
    tu_pump();
    if (GetForegroundWindow() != small) SKIP("cannot make a window the foreground window here");
    update_game_window(now_seconds());
    CHECK_EQ_PTR(g_gameWnd, small);
}

TEST(refresh_monitor_uses_the_game_windows_monitor_or_primary)
{
    wb_begin();
    POINT origin = {0, 0};
    MONITORINFO pm;
    pm.cbSize = sizeof(pm);
    REQUIRE(GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &pm));
    refresh_monitor(now_seconds()); // no window yet
    CHECK(EqualRect(&g_monitorRect, &pm.rcMonitor));
    HWND w = wb_window_ex(L"game", 0, 30, 40, 500, 300);
    g_windowSearchTime = -1.0;
    refresh_monitor(now_seconds());
    CHECK_EQ_PTR(g_gameWnd, w);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    REQUIRE(GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi));
    CHECK(EqualRect(&g_monitorRect, &mi.rcMonitor));
    CHECK(g_lastMonitor == MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST));
}

TEST(refresh_monitor_with_monitor_primary_ignores_windows)
{
    wb_begin();
    g_monitorPrimaryOnly = 1;
    wb_window_ex(L"game", 0, 30, 40, 500, 300);
    refresh_monitor(now_seconds());
    CHECK(g_gameWnd == NULL); // doesn't even look
    POINT origin = {0, 0};
    MONITORINFO pm;
    pm.cbSize = sizeof(pm);
    REQUIRE(GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &pm));
    CHECK(EqualRect(&g_monitorRect, &pm.rcMonitor));
}

// ===================================================================================== objects

TEST(create_device_and_destroy_directly)
{
    wb_begin();
    Device *d = NULL;
    CHECK_EQ_PTR(fac_CreateDevice(&g_factory, &d), &d);
    REQUIRE(d != NULL);
    CHECK_EQ_PTR(d->vtbl, g_devVtbl);
    CHECK_EQ_PTR(dev_Destroy(d, 0), d);
    CHECK_EQ_PTR(d->vtbl, g_devVtbl); // flags 0: not freed
    CHECK_EQ_PTR(dev_Destroy(d, 1), d);
}

TEST(create_weaver_initialises_the_control_block)
{
    wb_begin();
    SharedPtr sp = { (void *)1, (RefCount *)1 };
    CHECK_EQ_PTR(fac_CreateWeaver(&g_factory, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL), &sp);
    REQUIRE(sp.ptr && sp.rc);
    CHECK_EQ_PTR(sp.rc->obj, sp.ptr);
    CHECK_EQ_INT(sp.rc->uses, 1);
    CHECK_EQ_INT(sp.rc->weaks, 1);
    CHECK_EQ_INT(offsetof(RefCount, uses), 8);
    CHECK_EQ_INT(offsetof(RefCount, weaks), 12);
    CHECK_EQ_PTR(sp.rc->vtbl[0], rc_Destroy);
    CHECK_EQ_PTR(sp.rc->vtbl[1], rc_DeleteThis);
    CHECK_EQ_PTR(sp.rc->vtbl[2], rc_Dtor);
    CHECK_EQ_PTR(sp.rc->vtbl[3], rc_GetDeleter);
    CHECK_EQ_INT(sizeof(g_rcVtbl) / sizeof(g_rcVtbl[0]), 4);
    CHECK(rc_GetDeleter(sp.rc, "type") == NULL);
    CHECK_EQ_PTR(rc_Dtor(sp.rc, 0), sp.rc);
    rc_Destroy(sp.rc);
    CHECK(sp.rc->obj == NULL);
    rc_Destroy(sp.rc); // twice is harmless
    rc_DeleteThis(sp.rc);
}

int main(int argc, char **argv)
{
    tt_set_args(argc, argv);
    char path[TU_PATH * 3];
    tu_utf8(tu_dll_path(), path, sizeof(path));
    printf("DLL for the install-folder tests: %s%s\n", path, tu_running_under_wine() ? " (running under Wine)" : "");
    return tt_main(argc, argv);
}
