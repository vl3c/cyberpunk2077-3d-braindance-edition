// Tests of the RED4ext auto-setup plugin, src/braindance3d_autosetup.c (build/Braindance3D.dll).
//
// White box: this file #includes the .c to call auto_register() and its helpers against a scratch key
// HKCU\SOFTWARE\Braindance3D-Tests\<guid>\Samsung\Odyssey3DHub. RegSetValueExW and RegCreateKeyExW are
// redirected so tests can check the write order and inject failures.
// Black box: the built plugin is loaded for its exports, and Main() is run the way RED4ext runs it: in a copy
// of this program named <game>\bin\x64\Cyberpunk2077.exe, with the plugin at <game>\red4ext\plugins\Braindance3D.
// That child process redirects HKEY_CURRENT_USER to a scratch key (RegOverridePredefKey) and checks the
// redirection works before it calls Main, so the real HKCU\SOFTWARE\Samsung is never touched. Every Main test
// also checks the real key is unchanged afterwards.
//
// Run: test_autosetup.exe [--plugin path\to\Braindance3D.dll] [--dll path\to\Odyssey3DSDK.dll] [--list] [filters]

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------- fault injection into the code under test

static int fi_set_calls;           // RegSetValueExW calls since fi_reset()
static int fi_fail_set_at = -1;    // this call (0-based) fails with ERROR_ACCESS_DENIED
static int fi_fail_create;         // RegCreateKeyExW fails with ERROR_ACCESS_DENIED
static wchar_t fi_names[8][64];    // value names, in the order they were written

static void fi_reset(void)
{
    fi_set_calls = 0;
    fi_fail_set_at = -1;
    fi_fail_create = 0;
    memset(fi_names, 0, sizeof(fi_names));
}

static LONG WINAPI fi_RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE *data, DWORD size)
{
    int i = fi_set_calls++;
    if (i < 8) {
        wcsncpy(fi_names[i], name ? name : L"", 63);
        fi_names[i][63] = 0;
    }
    if (i == fi_fail_set_at) return ERROR_ACCESS_DENIED;
    return RegSetValueExW(key, name, reserved, type, data, size);
}

static LONG WINAPI fi_RegCreateKeyExW(HKEY key, LPCWSTR sub, DWORD reserved, LPWSTR cls, DWORD options, REGSAM sam,
                                      const LPSECURITY_ATTRIBUTES sa, PHKEY out, LPDWORD disposition)
{
    if (fi_fail_create) return ERROR_ACCESS_DENIED;
    return RegCreateKeyExW(key, sub, reserved, cls, options, sam, sa, out, disposition);
}

#define RegSetValueExW fi_RegSetValueExW
#define RegCreateKeyExW fi_RegCreateKeyExW
#include "../../src/braindance3d_autosetup.c"
#undef RegSetValueExW
#undef RegCreateKeyExW

#define TT_IMPLEMENTATION
#include "tinytest.h"
#include "testutil.h"

#ifndef BD3D_PRODUCT_NAME
#define BD3D_PRODUCT_NAME "3D Braindance Edition"
#endif

#define TEST_KEY_ROOT L"SOFTWARE\\Braindance3D-Tests"
#define LOG_NAME L"Braindance3D-autosetup.log"

typedef int (*PFN_Main)(HMODULE, uint8_t, const void *);
typedef void (*PFN_Query)(void *);
typedef uint32_t (*PFN_Supports)(void);

// ===================================================================================== the plugin under test

// The built plugin: --plugin <path>, else %BD3D_PLUGIN%, else ..\Braindance3D.dll next to this exe
// (build\tests\x.exe -> build\Braindance3D.dll), else build\Braindance3D.dll in the current folder.
static const wchar_t *plugin_path(void)
{
    static wchar_t path[TU_PATH];
    if (path[0]) return path;
    wchar_t cand[TU_PATH] = L"";
    const char *opt = tt_option("plugin");
    if (opt) {
        MultiByteToWideChar(CP_ACP, 0, opt, -1, cand, TU_PATH);
    } else if (GetEnvironmentVariableW(L"BD3D_PLUGIN", cand, TU_PATH) == 0) {
        wchar_t exe[TU_PATH];
        GetModuleFileNameW(NULL, exe, TU_PATH);
        wchar_t *s = wcsrchr(exe, L'\\');
        if (s) *s = 0;
        _snwprintf(cand, TU_PATH, L"%ls\\..\\Braindance3D.dll", exe);
        cand[TU_PATH - 1] = 0;
        if (!tu_exists(cand)) wcscpy(cand, L"build\\Braindance3D.dll");
    }
    if (!GetFullPathNameW(cand, TU_PATH, path, NULL)) wcscpy(path, cand);
    return path;
}

static void free_library_deferred(void *h) { FreeLibrary((HMODULE)h); }

static HMODULE plugin_load(void)
{
    REQUIRE_MSG(tu_exists(plugin_path()), "plugin under test not found: %ls (build it with ./build.sh, or pass --plugin)",
                plugin_path());
    HMODULE h = LoadLibraryExW(plugin_path(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    REQUIRE_MSG(h != NULL, "LoadLibrary(%ls) failed (%lu)", plugin_path(), GetLastError());
    tt_defer(free_library_deferred, h);
    return h;
}

// ===================================================================================== scratch registry keys

typedef struct {
    wchar_t root[256]; // SOFTWARE\Braindance3D-Tests\<guid>, relative to HKCU
    wchar_t hub[384];  // the key auto_register / Main writes, relative to HKCU
} Scratch;

static int scratch_is_safe(const wchar_t *path)
{
    size_t n = wcslen(TEST_KEY_ROOT L"\\");
    return _wcsnicmp(path, TEST_KEY_ROOT L"\\", n) == 0 && wcslen(path) > n + 30;
}

static void scratch_cleanup(void *p)
{
    Scratch *s = p;
    if (scratch_is_safe(s->root)) {
        RegDeleteTreeW(HKEY_CURRENT_USER, s->root);
        RegDeleteKeyW(HKEY_CURRENT_USER, s->root);
        RegDeleteKeyW(HKEY_CURRENT_USER, TEST_KEY_ROOT); // only succeeds when nothing else is under it
        HKEY k;
        CHECK_MSG(RegOpenKeyExW(HKEY_CURRENT_USER, s->root, 0, KEY_READ, &k) != ERROR_SUCCESS,
                  "scratch key HKCU\\%ls was not removed", s->root);
    }
    free(s);
}

// A fresh scratch key; `hub` is <root>\<hub_rel>. Removed after the test.
static Scratch *scratch_new(const wchar_t *hub_rel)
{
    Scratch *s = calloc(1, sizeof(*s));
    GUID g;
    REQUIRE(CoCreateGuid(&g) == S_OK);
    _snwprintf(s->root, 256, TEST_KEY_ROOT L"\\{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g.Data1, g.Data2,
               g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    _snwprintf(s->hub, 384, L"%ls\\%ls", s->root, hub_rel);
    REQUIRE(scratch_is_safe(s->root));
    tt_defer(scratch_cleanup, s);
    HKEY k;
    REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, s->root, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) == ERROR_SUCCESS);
    RegCloseKey(k);
    return s;
}

static void kset(const wchar_t *sub, const wchar_t *name, DWORD type, const void *data, DWORD size)
{
    REQUIRE(scratch_is_safe(sub));
    HKEY k;
    REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) == ERROR_SUCCESS);
    LONG rc = RegSetValueExW(k, name, 0, type, data, size);
    RegCloseKey(k);
    REQUIRE_EQ_INT(rc, ERROR_SUCCESS);
}

static void kset_sz(const wchar_t *sub, const wchar_t *name, const wchar_t *value)
{
    kset(sub, name, REG_SZ, value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

static int kexists(const wchar_t *sub)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    RegCloseKey(k);
    return 1;
}

// A string value (any string type, unexpanded). Returns its type, or 0 when the key or value is missing.
static DWORD kget(const wchar_t *sub, const wchar_t *name, wchar_t *out, DWORD cap)
{
    out[0] = 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, size = (cap - 1) * sizeof(wchar_t);
    LONG rc = RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &size);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        out[0] = 0;
        return 0;
    }
    out[size / sizeof(wchar_t)] = 0;
    return type ? type : REG_NONE;
}

static int kcount(const wchar_t *sub)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return -1;
    DWORD n = 0;
    RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, &n, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return (int)n;
}

// Every value of a key as text ("name=type:hex;..."), to check a key is unchanged. "<missing>" when absent.
static void kdump(const wchar_t *sub, char *out, size_t cap)
{
    out[0] = 0;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        snprintf(out, cap, "<missing>");
        return;
    }
    size_t len = 0;
    for (DWORD i = 0;; i++) {
        wchar_t name[256];
        BYTE data[2048];
        DWORD nlen = 256, dlen = sizeof(data), type = 0;
        if (RegEnumValueW(k, i, name, &nlen, NULL, &type, data, &dlen) != ERROR_SUCCESS) break;
        char n8[768];
        tu_utf8(name, n8, sizeof(n8));
        len += snprintf(out + len, len < cap ? cap - len : 0, "%s=%lu:", n8, type);
        for (DWORD j = 0; j < dlen && len + 3 < cap; j++) len += snprintf(out + len, cap - len, "%02x", data[j]);
        if (len + 2 < cap) out[len++] = ';', out[len] = 0;
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- the real key must never change

static unsigned long long fnv(unsigned long long h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static unsigned long long hash_key(HKEY parent, const wchar_t *sub, unsigned long long h, int depth)
{
    HKEY k;
    if (RegOpenKeyExW(parent, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return fnv(h, "<missing>", 9);
    h = fnv(h, sub, wcslen(sub) * sizeof(wchar_t));
    for (DWORD i = 0;; i++) {
        wchar_t name[256];
        BYTE data[4096];
        DWORD nlen = 256, dlen = sizeof(data), type = 0;
        LONG rc = RegEnumValueW(k, i, name, &nlen, NULL, &type, data, &dlen);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        h = fnv(h, name, nlen * sizeof(wchar_t));
        h = fnv(h, &type, sizeof(type));
        h = fnv(h, data, dlen);
    }
    for (DWORD i = 0; depth < 8; i++) {
        wchar_t name[256];
        DWORD nlen = 256;
        if (RegEnumKeyExW(k, i, name, &nlen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        h = hash_key(k, name, h, depth + 1);
    }
    RegCloseKey(k);
    return h;
}

static unsigned long long real_samsung_hash(void)
{
    return hash_key(HKEY_CURRENT_USER, L"SOFTWARE\\Samsung", 1469598103934665603ull, 0);
}

static void check_real_samsung_unchanged(void *p)
{
    unsigned long long *before = p;
    CHECK_MSG(real_samsung_hash() == *before, "the real HKCU\\SOFTWARE\\Samsung key changed during this test");
    free(before);
}

static void guard_real_samsung(void)
{
    unsigned long long *before = malloc(sizeof(*before));
    *before = real_samsung_hash();
    tt_defer(check_real_samsung_unchanged, before);
}

// ===================================================================================== SDK folders

enum { SDK_OURS, SDK_FOREIGN_NO_VERSION, SDK_FOREIGN_PRODUCT };

static void copy_file(const wchar_t *from, const wchar_t *to)
{
    REQUIRE_MSG(CopyFileW(from, to, FALSE), "copy %ls -> %ls failed (%lu)", from, to, GetLastError());
}

// Puts an Odyssey3DSDK.dll at <dir>\GameLauncherData\core: this mod's, a DLL with no version resource, or a
// DLL with another product's version resource (a copy of a Windows DLL, standing in for Samsung's).
static void put_sdk(const wchar_t *dir, int kind)
{
    wchar_t core[TU_PATH], dll[TU_PATH];
    _snwprintf(core, TU_PATH, L"%ls\\GameLauncherData\\core", dir);
    REQUIRE(tu_mkdirs(core));
    tu_join(dll, TU_PATH, core, L"Odyssey3DSDK.dll");
    if (kind == SDK_OURS) {
        tu_require_dll();
        copy_file(tu_dll_path(), dll);
    } else if (kind == SDK_FOREIGN_NO_VERSION) {
        static const char stub[] = "MZ not really a DLL, and no version resource";
        REQUIRE(tu_write_file(dll, stub, sizeof(stub)));
    } else {
        wchar_t sys[TU_PATH];
        GetSystemDirectoryW(sys, TU_PATH);
        wcscat(sys, L"\\version.dll");
        copy_file(sys, dll);
    }
}

static void put_settings(const wchar_t *dir)
{
    wchar_t ini[TU_PATH];
    tu_join(ini, TU_PATH, dir, SETTINGS_FILE);
    REQUIRE(tu_write_text(ini, "[Log]\r\nLevel=0\r\n"));
}

static void sub_dir(wchar_t *out, const wchar_t *base, const wchar_t *rel, int create)
{
    _snwprintf(out, TU_PATH, L"%ls\\%ls", base, rel);
    out[TU_PATH - 1] = 0;
    if (create) REQUIRE(tu_mkdirs(out));
}

// ===================================================================================== white box: fixture

typedef struct {
    wchar_t *tmp;
    Scratch *reg;
    const wchar_t *hub;
    wchar_t target[TU_PATH];  // <tmp>\game\bin\x64\Braindance3D, with this mod's SDK
    wchar_t samsung[TU_PATH]; // <tmp>\Program Files\Odyssey 3D Hub, not created
    wchar_t log[TU_PATH];
} Ctx;

static void wb_close_log(void *unused)
{
    if (g_log) fclose(g_log);
    g_log = NULL;
}

static Ctx *ctx_new(const wchar_t *tag, int with_sdk)
{
    Ctx *c = calloc(1, sizeof(*c));
    tt_defer(free, c);
    wb_close_log(NULL);
    fi_reset();
    c->tmp = tu_tempdir(tag);
    c->reg = scratch_new(L"Samsung\\Odyssey3DHub");
    c->hub = c->reg->hub;
    sub_dir(c->target, c->tmp, L"game\\bin\\x64\\Braindance3D", 1);
    if (with_sdk) put_sdk(c->target, SDK_OURS);
    sub_dir(c->samsung, c->tmp, L"Program Files\\Odyssey 3D Hub", 0);
    // Send the plugin's log lines to a file, so every test also runs the logging code.
    tu_join(c->log, TU_PATH, c->tmp, L"wb.log");
    g_log = _wfopen(c->log, L"w");
    REQUIRE(g_log != NULL);
    tt_defer(wb_close_log, NULL);
    return c;
}

// Runs auto_register and checks it explained itself in the log.
static AutoResult run_auto(Ctx *c, const wchar_t *target, const wchar_t *samsung)
{
    fflush(g_log);
    long long before = tu_file_size(c->log);
    fi_set_calls = 0;
    memset(fi_names, 0, sizeof(fi_names));
    AutoResult r = auto_register(c->hub, target, samsung);
    fflush(g_log);
    CHECK_MSG(tu_file_size(c->log) > before, "auto_register returned %d without logging why", (int)r);
    return r;
}

static void check_value(const wchar_t *sub, const wchar_t *name, const wchar_t *want)
{
    wchar_t got[2048];
    DWORD type = kget(sub, name, got, 2048);
    CHECK_MSG(type == REG_SZ, "%ls: type %lu, want REG_SZ", name, type);
    CHECK_MSG(wcscmp(got, want) == 0, "%ls = '%ls', want '%ls'", name, got, want);
}

static void check_marker(const wchar_t *sub)
{
    wchar_t got[2048];
    CHECK_EQ_INT(kget(sub, VALUE_MARKER, got, 2048), REG_SZ);
    CHECK_MSG(wcsncmp(got, L"" BD3D_VERSION_STR, wcslen(L"" BD3D_VERSION_STR)) == 0,
              "Braindance3D_InstalledBy = '%ls' does not start with the version " BD3D_VERSION_STR, got);
}

// The four values an install leaves, pointing at target.
static void check_installed(const wchar_t *sub, const wchar_t *target, const wchar_t *previous)
{
    check_value(sub, VALUE_INSTALL_PATH, target);
    check_value(sub, VALUE_OWN_PATH, target);
    check_value(sub, VALUE_PREVIOUS, previous);
    check_marker(sub);
}

// ===================================================================================== white box: one function at a time

static int log_has(Ctx *c, const char *what)
{
    fflush(g_log);
    char *text = tu_read_file(c->log, NULL);
    int has = text && strstr(text, what) != NULL;
    free(text);
    return has;
}

TEST(utf8_handles_null_and_small_buffers)
{
    char buf[16];
    CHECK_EQ_STR(utf8(L"C:\\Games", buf, sizeof(buf)), "C:\\Games");
    CHECK_EQ_STR(utf8(NULL, buf, sizeof(buf)), "");
    CHECK_EQ_STR(utf8(L"a path longer than sixteen", buf, sizeof(buf)), "");
}

TEST(open_append_log_appends_and_starts_over_past_256_kb)
{
    wchar_t *dir = tu_tempdir(L"append");
    wchar_t path[TU_PATH];
    tu_join(path, TU_PATH, dir, L"a.log");
    REQUIRE(tu_write_text(path, "kept\n"));
    FILE *f = open_append_log(path);
    REQUIRE(f != NULL);
    fputs("added\n", f);
    fclose(f);
    char *text = tu_read_file(path, NULL);
    REQUIRE(text != NULL);
    CHECK(strncmp(text, "kept\n", 5) == 0);
    CHECK(strstr(text, "added") != NULL);
    free(text);
    char *big = malloc(300 * 1024);
    memset(big, 'x', 300 * 1024);
    REQUIRE(tu_write_file(path, big, 300 * 1024));
    free(big);
    f = open_append_log(path);
    REQUIRE(f != NULL);
    fputs("fresh\n", f);
    fclose(f);
    CHECK(tu_file_size(path) < 16);
    wchar_t missing[TU_PATH];
    tu_join(missing, TU_PATH, dir, L"no such folder\\a.log");
    CHECK(open_append_log(missing) == NULL);
}

TEST(file_exists_is_only_for_files)
{
    wchar_t *dir = tu_tempdir(L"exists");
    wchar_t file[TU_PATH], missing[TU_PATH];
    tu_join(file, TU_PATH, dir, L"f.txt");
    tu_join(missing, TU_PATH, dir, L"none.txt");
    REQUIRE(tu_write_text(file, "x"));
    CHECK_EQ_INT(file_exists(file), 1);
    CHECK_EQ_INT(file_exists(dir), 0);
    CHECK_EQ_INT(file_exists(missing), 0);
}

TEST(definitely_missing_only_for_a_local_path_that_isnt_there)
{
    wchar_t *dir = tu_tempdir(L"missing");
    wchar_t gone[TU_PATH], goneDeep[TU_PATH], quoted[TU_PATH];
    tu_join(gone, TU_PATH, dir, L"nothing here");
    tu_join(goneDeep, TU_PATH, dir, L"no folder\\nor file.dll");
    _snwprintf(quoted, TU_PATH, L"\"%ls\"", gone);
    CHECK_EQ_INT(definitely_missing(gone), 1);
    CHECK_EQ_INT(definitely_missing(goneDeep), 1);
    CHECK_EQ_INT(definitely_missing(dir), 0); // there
    CHECK_EQ_INT(definitely_missing(quoted), 0);
    CHECK_EQ_INT(definitely_missing(L"\\\\server\\share\\x.dll"), 0); // a network path: unknown
    CHECK_EQ_INT(definitely_missing(L""), 0);
    CHECK_EQ_INT(definitely_missing(NULL), 0);
}

TEST(sdk_path_is_under_game_launcher_data_core)
{
    wchar_t out[MAX_PATH];
    CHECK_EQ_INT(sdk_path(L"C:\\Games\\X", out), 1);
    CHECK_EQ_WSTR(out, L"C:\\Games\\X\\GameLauncherData\\core\\Odyssey3DSDK.dll");
    wchar_t longDir[MAX_PATH];
    for (int i = 0; i < MAX_PATH - 10; i++) longDir[i] = L'd';
    longDir[MAX_PATH - 10] = 0;
    CHECK_EQ_INT(sdk_path(longDir, out), 0);
}

TEST(reg_set_writes_a_terminated_string)
{
    Scratch *s = scratch_new(L"set");
    HKEY k;
    REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, s->hub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) == ERROR_SUCCESS);
    CHECK_EQ_INT(reg_set(k, L"v", L"C:\\Games"), ERROR_SUCCESS);
    RegCloseKey(k);
    wchar_t got[64];
    CHECK_EQ_INT(kget(s->hub, L"v", got, 64), REG_SZ);
    CHECK_EQ_WSTR(got, L"C:\\Games");
}

TEST(read_reg_state_reads_the_four_values)
{
    Ctx *c = ctx_new(L"state", 0);
    RegState st;
    memset(&st, 'x', sizeof(st));
    REQUIRE(!kexists(c->hub));
    CHECK_EQ_INT(read_reg_state(c->hub, &st), AUTO_GO_ON); // no key yet: all empty
    CHECK_EQ_WSTR(st.current, L"");
    CHECK_EQ_WSTR(st.marker, L"");
    kset_sz(c->hub, VALUE_INSTALL_PATH, L"C:\\A");
    kset_sz(c->hub, VALUE_OWN_PATH, L"C:\\B");
    kset_sz(c->hub, VALUE_PREVIOUS, L"C:\\C");
    kset_sz(c->hub, VALUE_MARKER, L"0.2.0");
    CHECK_EQ_INT(read_reg_state(c->hub, &st), AUTO_GO_ON);
    CHECK_EQ_WSTR(st.current, L"C:\\A");
    CHECK_EQ_WSTR(st.own, L"C:\\B");
    CHECK_EQ_WSTR(st.previous, L"C:\\C");
    CHECK_EQ_WSTR(st.marker, L"0.2.0");
    log_reg_state(&st);
    CHECK(log_has(c, "Registry before: InstallPath='C:\\A' ownPath='C:\\B' previous='C:\\C' installedBy='0.2.0'"));
}

TEST(read_reg_state_stops_on_values_it_cant_read)
{
    Ctx *c = ctx_new(L"state-bad", 0);
    RegState st;
    DWORD dw = 1;
    kset(c->hub, VALUE_PREVIOUS, REG_DWORD, &dw, sizeof(dw));
    CHECK_EQ_INT(read_reg_state(c->hub, &st), AUTO_ERROR);
    CHECK(log_has(c, "The saved previous InstallPath can't be read"));
    kset(c->hub, VALUE_INSTALL_PATH, REG_DWORD, &dw, sizeof(dw));
    CHECK_EQ_INT(read_reg_state(c->hub, &st), AUTO_FOREIGN_SDK);
    CHECK(log_has(c, "InstallPath is set but isn't a readable path"));
}

TEST(samsung_hub_in_wants_an_sdk_that_isnt_this_mods)
{
    Ctx *c = ctx_new(L"hub", 1);
    CHECK_EQ_INT(samsung_hub_in(NULL), 0);
    CHECK_EQ_INT(samsung_hub_in(L""), 0);
    CHECK_EQ_INT(samsung_hub_in(c->samsung), 0); // not there
    CHECK_EQ_INT(samsung_hub_in(c->target), 0);  // this mod's
    put_sdk(c->samsung, SDK_FOREIGN_PRODUCT);
    CHECK_EQ_INT(samsung_hub_in(c->samsung), 1);
}

TEST(decide_over_current_cases)
{
    Ctx *c = ctx_new(L"decide", 1);
    wchar_t foreign[TU_PATH], gone[TU_PATH];
    sub_dir(foreign, c->tmp, L"foreign", 1);
    put_sdk(foreign, SDK_FOREIGN_PRODUCT);
    sub_dir(gone, c->tmp, L"gone", 0);
    RegState st;
    const wchar_t *keep = NULL;

    memset(&st, 0, sizeof(st)); // nothing registered, no Samsung Hub
    CHECK_EQ_INT(decide_over_current(&st, c->samsung, &keep), AUTO_GO_ON);
    CHECK_EQ_WSTR(keep, NO_PREVIOUS);

    wcscpy(st.current, c->target); // another copy of this mod, with what it saved
    wcscpy(st.previous, L"C:\\Before");
    CHECK_EQ_INT(decide_over_current(&st, c->samsung, &keep), AUTO_GO_ON);
    CHECK_EQ_WSTR(keep, L"C:\\Before");

    memset(&st, 0, sizeof(st)); // someone else's SDK
    wcscpy(st.current, foreign);
    CHECK_EQ_INT(decide_over_current(&st, c->samsung, &keep), AUTO_FOREIGN_SDK);

    wcscpy(st.current, gone); // a folder that's gone: saved for the uninstaller
    CHECK_EQ_INT(decide_over_current(&st, c->samsung, &keep), AUTO_GO_ON);
    CHECK_EQ_WSTR(keep, gone);

    put_sdk(c->samsung, SDK_FOREIGN_PRODUCT); // ... unless Samsung's Hub is in its default folder
    CHECK_EQ_INT(decide_over_current(&st, c->samsung, &keep), AUTO_SAMSUNG_DEFAULT);
    st.current[0] = 0;
    CHECK_EQ_INT(decide_over_current(&st, c->samsung, &keep), AUTO_SAMSUNG_DEFAULT);
}

TEST(write_registration_writes_the_bookkeeping_then_install_path)
{
    Ctx *c = ctx_new(L"write", 1);
    CHECK_EQ_INT(write_registration(c->hub, c->target, L"C:\\Before"), AUTO_INSTALLED);
    check_installed(c->hub, c->target, L"C:\\Before");
    CHECK_EQ_WSTR(fi_names[0], VALUE_PREVIOUS);
    CHECK_EQ_WSTR(fi_names[3], VALUE_INSTALL_PATH);
    CHECK(log_has(c, "Registered: InstallPath='"));
    fi_reset();
    fi_fail_create = 1;
    CHECK_EQ_INT(write_registration(c->hub, c->target, NO_PREVIOUS), AUTO_ERROR);
    CHECK(log_has(c, "Couldn't open or create HKCU\\"));
}

// ===================================================================================== white box: ABI

TEST(plugin_info_struct_matches_the_red4ext_v0_layout)
{
    // RED4ext.SDK include/RED4ext/Api/v0: SemVer {u8 major; u16 minor; u32 patch; {u32 type; u32 number}},
    // FileVer {u16 x4}, PluginInfo {SemVer sdk; const wchar_t *name, *author; SemVer version; FileVer runtime}.
    CHECK_EQ_INT(sizeof(R4eSemVer), 16);
    CHECK_EQ_INT(offsetof(R4eSemVer, minor), 2);
    CHECK_EQ_INT(offsetof(R4eSemVer, patch), 4);
    CHECK_EQ_INT(offsetof(R4eSemVer, prerelease), 8);
    CHECK_EQ_INT(sizeof(R4eFileVer), 8);
    CHECK_EQ_INT(sizeof(R4ePluginInfo), 56);
    CHECK_EQ_INT(offsetof(R4ePluginInfo, name), 16);
    CHECK_EQ_INT(offsetof(R4ePluginInfo, author), 24);
    CHECK_EQ_INT(offsetof(R4ePluginInfo, version), 32);
    CHECK_EQ_INT(offsetof(R4ePluginInfo, runtime), 48);
}

TEST(registry_names_match_the_installer_script)
{
    // Braindance3D-Setup.ps1 reads and undoes what the plugin writes; the Pester tests check the script's side.
    CHECK_EQ_WSTR(DEFAULT_KEY, L"SOFTWARE\\Samsung\\Odyssey3DHub");
    CHECK_EQ_WSTR(VALUE_INSTALL_PATH, L"InstallPath");
    CHECK_EQ_WSTR(VALUE_MARKER, L"Braindance3D_InstalledBy");
    CHECK_EQ_WSTR(VALUE_OWN_PATH, L"Braindance3D_InstallPath");
    CHECK_EQ_WSTR(VALUE_PREVIOUS, L"Braindance3D_PreviousInstallPath");
    CHECK_EQ_WSTR(NO_PREVIOUS, L"<none>");
    CHECK_EQ_WSTR(SETTINGS_FILE, L"Braindance3D-Settings.ini");
    CHECK_EQ_WSTR(PRODUCT_NAME, L"" BD3D_PRODUCT_NAME);
}

// ===================================================================================== white box: helpers

TEST(normalize_resolves_and_trims_trailing_separators_but_keeps_drive_roots)
{
    struct { const wchar_t *in, *want; } cases[] = {
        { L"C:\\", L"C:\\" },
        { L"C:\\a\\", L"C:\\a" },
        { L"C:\\a\\\\", L"C:\\a" },
        { L"C:/a/b/", L"C:\\a\\b" },
        { L"C:\\a\\.\\b\\..\\c", L"C:\\a\\c" },
        { L"D:\\Games\\Cyberpunk 2077\\bin\\x64\\Braindance3D", L"D:\\Games\\Cyberpunk 2077\\bin\\x64\\Braindance3D" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        wchar_t out[MAX_PATH];
        CHECK_MSG(normalize(cases[i].in, out), "normalize(%ls) failed", cases[i].in);
        CHECK_EQ_WSTR(out, cases[i].want);
    }
    wchar_t lng[600], out[MAX_PATH];
    wcscpy(lng, L"C:\\");
    for (int i = 0; i < 300; i++) wcscat(lng, L"a");
    CHECK_EQ_INT(normalize(lng, out), 0);
}

TEST(same_path_ignores_case_separators_and_trailing_backslashes)
{
    CHECK(same_path(L"C:\\Games\\X", L"c:\\games\\x"));
    CHECK(same_path(L"C:\\Games\\X\\", L"C:/Games/X"));
    CHECK(same_path(L"C:\\Games\\.\\X", L"C:\\Games\\X"));
    CHECK(!same_path(L"C:\\Games\\X", L"C:\\Games\\X2"));
    CHECK(!same_path(L"C:\\Games\\X", L"D:\\Games\\X"));
    CHECK(!same_path(NULL, L"C:\\a"));
    CHECK(!same_path(L"C:\\a", NULL));
    CHECK(!same_path(L"", L""));
    CHECK(!same_path(L"", L"C:\\a"));
    wchar_t lng[600];
    wcscpy(lng, L"C:\\");
    for (int i = 0; i < 300; i++) wcscat(lng, L"a");
    CHECK(!same_path(lng, lng)); // too long to normalise: never "the same"
}

TEST(sdk_info_tells_this_mod_from_other_sdks)
{
    wchar_t *tmp = tu_tempdir(L"sdkinfo");
    wchar_t ours[TU_PATH], bysettings[TU_PATH], foreign[TU_PATH], product[TU_PATH], empty[TU_PATH], missing[TU_PATH];
    sub_dir(ours, tmp, L"ours", 1);
    put_sdk(ours, SDK_OURS);
    sub_dir(bysettings, tmp, L"by settings", 1);
    put_sdk(bysettings, SDK_FOREIGN_NO_VERSION);
    put_settings(bysettings);
    sub_dir(foreign, tmp, L"foreign", 1);
    put_sdk(foreign, SDK_FOREIGN_NO_VERSION);
    sub_dir(product, tmp, L"product", 1);
    put_sdk(product, SDK_FOREIGN_PRODUCT);
    sub_dir(empty, tmp, L"empty", 1);
    sub_dir(missing, tmp, L"missing", 0);

    SdkInfo i = sdk_info(ours);
    CHECK(i.exists && i.ours);
    CHECK_EQ_WSTR(i.product, L"" BD3D_PRODUCT_NAME);
    i = sdk_info(bysettings);
    CHECK(i.exists && i.ours);
    i = sdk_info(foreign);
    CHECK(i.exists && !i.ours);
    CHECK_EQ_WSTR(i.product, L"");
    i = sdk_info(product);
    CHECK(i.exists && !i.ours);
    tt_note("a Windows DLL's product name: '%ls'", i.product);
    i = sdk_info(empty);
    CHECK(!i.exists && !i.ours);
    i = sdk_info(missing);
    CHECK(!i.exists && !i.ours);
    i = sdk_info(L"");
    CHECK(!i.exists);
    i = sdk_info(NULL);
    CHECK(!i.exists);
    wchar_t lng[600];
    wcscpy(lng, tmp);
    wcscat(lng, L"\\");
    while (wcslen(lng) < 300) wcscat(lng, L"x");
    i = sdk_info(lng);
    CHECK(!i.exists);
}

TEST(product_name_is_empty_without_a_version_resource)
{
    wchar_t *tmp = tu_tempdir(L"product");
    wchar_t f[TU_PATH], out[128];
    tu_join(f, TU_PATH, tmp, L"stub.dll");
    REQUIRE(tu_write_text(f, "MZ"));
    wcscpy(out, L"junk");
    product_name(f, out, 128);
    CHECK_EQ_WSTR(out, L"");
    tu_join(f, TU_PATH, tmp, L"does-not-exist.dll");
    wcscpy(out, L"junk");
    product_name(f, out, 128);
    CHECK_EQ_WSTR(out, L"");
    tu_require_dll();
    product_name(tu_dll_path(), out, 128);
    CHECK_EQ_WSTR(out, L"" BD3D_PRODUCT_NAME);
    wchar_t small[8];
    product_name(tu_dll_path(), small, 8); // truncated, still terminated
    CHECK_EQ_INT(wcslen(small), 7);
}

TEST(reg_get_reads_string_values_only)
{
    Scratch *s = scratch_new(L"values");
    DWORD dw = 42;
    kset(s->hub, L"dword", REG_DWORD, &dw, sizeof(dw));
    kset_sz(s->hub, L"sz", L"C:\\Games\\X");
    kset_sz(s->hub, L"empty", L"");
    kset(s->hub, L"unterminated", REG_SZ, L"C:\\abc", 6 * sizeof(wchar_t));
    kset(s->hub, L"expand", REG_EXPAND_SZ, L"%TEMP%\\x", 9 * sizeof(wchar_t));
    wchar_t max[MAX_PATH];
    for (int i = 0; i < MAX_PATH - 1; i++) max[i] = L'a' + i % 26;
    max[MAX_PATH - 1] = 0;
    kset_sz(s->hub, L"max", max);
    wchar_t tooLong[MAX_PATH + 40];
    for (int i = 0; i < MAX_PATH + 39; i++) tooLong[i] = L'z';
    tooLong[MAX_PATH + 39] = 0;
    kset_sz(s->hub, L"toolong", tooLong);

    HKEY k;
    REQUIRE(RegOpenKeyExW(HKEY_CURRENT_USER, s->hub, 0, KEY_READ, &k) == ERROR_SUCCESS);
    wchar_t out[MAX_PATH];
    CHECK_EQ_INT(reg_get(k, L"missing", out), 0);
    CHECK_EQ_WSTR(out, L"");
    CHECK_EQ_INT(reg_get(k, L"dword", out), -1); // present but not a string: never treated as unset
    CHECK_EQ_WSTR(out, L"");
    CHECK_EQ_INT(reg_get(k, L"sz", out), 1);
    CHECK_EQ_WSTR(out, L"C:\\Games\\X");
    CHECK_EQ_INT(reg_get(k, L"empty", out), 1);
    CHECK_EQ_WSTR(out, L"");
    CHECK_EQ_INT(reg_get(k, L"unterminated", out), 1);
    CHECK_EQ_WSTR(out, L"C:\\abc");
    CHECK_EQ_INT(reg_get(k, L"expand", out), 1);
    wchar_t expanded[MAX_PATH];
    ExpandEnvironmentStringsW(L"%TEMP%\\x", expanded, MAX_PATH);
    CHECK_EQ_WSTR(out, expanded);
    CHECK_EQ_INT(reg_get(k, L"max", out), 1);
    CHECK_EQ_WSTR(out, max);
    CHECK_EQ_INT(reg_get(k, L"toolong", out), -1);
    CHECK_EQ_WSTR(out, L"");
    RegCloseKey(k);
    CHECK_EQ_INT(reg_get(NULL, L"sz", out), 0);
}

// ===================================================================================== white box: logging

TEST(open_log_appends_to_the_log_in_the_target_folder)
{
    wb_close_log(NULL);
    tt_defer(wb_close_log, NULL);
    wchar_t *dir = tu_tempdir(L"log");
    wchar_t log[TU_PATH];
    tu_join(log, TU_PATH, dir, LOG_NAME);
    REQUIRE(tu_write_text(log, "earlier run\n"));
    open_log(dir);
    REQUIRE(g_log != NULL);
    alog("one %d", 1);
    alog("two %s", "2");
    wb_close_log(NULL);
    char *text = tu_read_file(log, NULL);
    REQUIRE(text);
    CHECK(strncmp(text, "earlier run\n", 12) == 0);
    CHECK_EQ_INT(tu_count_lines(text), 3);
    free(text);
}

TEST(open_log_starts_over_once_the_log_passes_256_kb)
{
    wb_close_log(NULL);
    tt_defer(wb_close_log, NULL);
    wchar_t *dir = tu_tempdir(L"logbig");
    wchar_t log[TU_PATH];
    tu_join(log, TU_PATH, dir, LOG_NAME);
    size_t big = 300 * 1024;
    char *junk = malloc(big);
    memset(junk, 'x', big);
    REQUIRE(tu_write_file(log, junk, big));
    open_log(dir);
    REQUIRE(g_log != NULL);
    alog("fresh");
    wb_close_log(NULL);
    long long size = tu_file_size(log);
    CHECK_MSG(size > 0 && size < 4096, "log is %lld bytes after starting over", size);

    // Under the limit it is kept.
    size_t small = 200 * 1024;
    REQUIRE(tu_write_file(log, junk, small));
    open_log(dir);
    REQUIRE(g_log != NULL);
    alog("appended");
    wb_close_log(NULL);
    CHECK(tu_file_size(log) > (long long)small);
    free(junk);
}

TEST(open_log_falls_back_to_localappdata_when_the_folder_is_missing)
{
    wb_close_log(NULL);
    tt_defer(wb_close_log, NULL);
    wchar_t *tmp = tu_tempdir(L"logfb");
    wchar_t missing[TU_PATH], lad[TU_PATH], log[TU_PATH];
    sub_dir(missing, tmp, L"no such folder", 0);
    sub_dir(lad, tmp, L"localappdata " TU_NONASCII, 1);
    tu_setenv(L"LOCALAPPDATA", lad);
    open_log(missing);
    REQUIRE(g_log != NULL);
    alog("fallback");
    wb_close_log(NULL);
    _snwprintf(log, TU_PATH, L"%ls\\Braindance3D\\" LOG_NAME, lad);
    CHECK(tu_file_size(log) > 0);
    CHECK(!tu_exists(missing));
}

TEST(open_log_with_nowhere_to_write_logs_nothing_and_alog_is_safe)
{
    wb_close_log(NULL);
    tt_defer(wb_close_log, NULL);
    wchar_t *tmp = tu_tempdir(L"lognone");
    wchar_t missing[TU_PATH], file[TU_PATH], under_file[TU_PATH], lng[600];
    sub_dir(missing, tmp, L"no such folder", 0);

    tu_setenv(L"LOCALAPPDATA", NULL);
    open_log(missing);
    CHECK(g_log == NULL);
    alog("dropped %s", "quietly");

    tu_join(file, TU_PATH, tmp, L"a file");
    REQUIRE(tu_write_text(file, "x"));
    tu_join(under_file, TU_PATH, file, L"localappdata");
    tu_setenv(L"LOCALAPPDATA", under_file);
    open_log(missing);
    CHECK(g_log == NULL);

    wcscpy(lng, tmp);
    wcscat(lng, L"\\");
    while (wcslen(lng) < 240) wcscat(lng, L"y");
    tu_setenv(L"LOCALAPPDATA", lng);
    open_log(missing);
    CHECK(g_log == NULL);
    alog("dropped");

    // A target folder name too long for the log path also falls back (here: to nothing).
    open_log(lng);
    CHECK(g_log == NULL);
}

// ===================================================================================== white box: defaults

TEST(default_target_is_braindance3d_next_to_the_exe)
{
    wchar_t t[MAX_PATH], exe[MAX_PATH];
    REQUIRE(default_target(t));
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    *wcsrchr(exe, L'\\') = 0;
    wcscat(exe, L"\\Braindance3D");
    CHECK_EQ_WSTR(t, exe);
}

TEST(default_samsung_dir_is_odyssey_3d_hub_under_program_files)
{
    wchar_t out[MAX_PATH];
    tu_setenv(L"ProgramFiles", L"C:\\Program Files");
    default_samsung_dir(out);
    CHECK_EQ_WSTR(out, L"C:\\Program Files\\Odyssey 3D Hub");
    tu_setenv(L"ProgramFiles", NULL);
    default_samsung_dir(out);
    CHECK_EQ_WSTR(out, L"");
    wchar_t lng[300] = L"C:\\";
    while (wcslen(lng) < 250) wcscat(lng, L"p");
    tu_setenv(L"ProgramFiles", lng);
    default_samsung_dir(out);
    CHECK_EQ_WSTR(out, L"");
}

// ===================================================================================== white box: auto_register

TEST(auto_without_the_sdk_changes_nothing)
{
    Ctx *c = ctx_new(L"nosdk", 0);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_NO_SDK);
    CHECK(!kexists(c->hub));
    CHECK_EQ_INT(fi_set_calls, 0);

    // Also when something is registered: nothing is read or written.
    kset_sz(c->hub, VALUE_INSTALL_PATH, L"C:\\Elsewhere");
    char before[8192], after[8192];
    kdump(c->hub, before, sizeof(before));
    wchar_t missing[TU_PATH];
    sub_dir(missing, c->tmp, L"not extracted", 0);
    CHECK_EQ_INT(run_auto(c, missing, c->samsung), AUTO_NO_SDK);
    kdump(c->hub, after, sizeof(after));
    CHECK_EQ_STR(after, before);
}

TEST(auto_fresh_install_writes_install_path_and_bookkeeping)
{
    Ctx *c = ctx_new(L"fresh", 1);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    check_installed(c->hub, c->target, NO_PREVIOUS);
    CHECK_EQ_INT(kcount(c->hub), 4);
}

TEST(auto_stores_the_target_normalized)
{
    Ctx *c = ctx_new(L"norm", 1);
    wchar_t variants[3][TU_PATH];
    _snwprintf(variants[0], TU_PATH, L"%ls\\", c->target);
    _snwprintf(variants[1], TU_PATH, L"%ls\\game\\.\\bin\\x64\\..\\x64\\Braindance3D", c->tmp);
    wcscpy(variants[2], c->target);
    for (wchar_t *p = variants[2]; *p; p++)
        if (*p == L'\\') *p = L'/';
    for (int i = 0; i < 3; i++) {
        RegDeleteTreeW(HKEY_CURRENT_USER, c->reg->root);
        CHECK_EQ_INT(run_auto(c, variants[i], c->samsung), AUTO_INSTALLED);
        check_value(c->hub, VALUE_INSTALL_PATH, c->target);
        check_value(c->hub, VALUE_OWN_PATH, c->target);
    }
}

TEST(auto_already_registered_changes_nothing)
{
    Ctx *c = ctx_new(L"already", 1);
    wchar_t variants[4][TU_PATH];
    wcscpy(variants[0], c->target);
    wcscpy(variants[1], c->target);
    _wcsupr(variants[1]);
    _snwprintf(variants[2], TU_PATH, L"%ls\\", c->target);
    wcscpy(variants[3], c->target);
    for (wchar_t *p = variants[3]; *p; p++)
        if (*p == L'\\') *p = L'/';
    for (int i = 0; i < 4; i++) {
        RegDeleteTreeW(HKEY_CURRENT_USER, c->reg->root);
        kset_sz(c->hub, VALUE_INSTALL_PATH, variants[i]);
        char before[8192], after[8192];
        kdump(c->hub, before, sizeof(before));
        CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_ALREADY);
        CHECK_EQ_INT(fi_set_calls, 0);
        kdump(c->hub, after, sizeof(after));
        CHECK_EQ_STR(after, before);
    }
}

TEST(auto_replaces_a_dead_install_path_and_saves_it_for_uninstall)
{
    Ctx *c = ctx_new(L"dead", 1);
    wchar_t gone[TU_PATH], empty[TU_PATH];
    sub_dir(gone, c->tmp, L"deleted hub", 0);
    kset_sz(c->hub, VALUE_INSTALL_PATH, gone);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    check_installed(c->hub, c->target, gone);

    // A folder that exists but has no SDK in it is just as unused.
    RegDeleteTreeW(HKEY_CURRENT_USER, c->reg->root);
    sub_dir(empty, c->tmp, L"empty hub", 1);
    kset_sz(c->hub, VALUE_INSTALL_PATH, empty);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    check_installed(c->hub, c->target, empty);
}

TEST(auto_dead_install_path_is_kept_when_samsung_is_in_its_default_folder)
{
    // With InstallPath gone the game may fall back to Samsung's default folder, so that SDK wins.
    Ctx *c = ctx_new(L"deadsam", 1);
    REQUIRE(tu_mkdirs(c->samsung));
    put_sdk(c->samsung, SDK_FOREIGN_NO_VERSION);
    wchar_t gone[TU_PATH];
    sub_dir(gone, c->tmp, L"deleted", 0);
    kset_sz(c->hub, VALUE_INSTALL_PATH, gone);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_SAMSUNG_DEFAULT);
    wchar_t got[TU_PATH];
    kget(c->hub, VALUE_INSTALL_PATH, got, TU_PATH);
    CHECK_EQ_WSTR(got, gone);
}

TEST(auto_install_path_on_a_missing_drive_is_not_replaced)
{
    // An offline USB or network drive looks like "not found": that must never count as "gone".
    Ctx *c = ctx_new(L"nodrive", 1);
    const wchar_t *candidates = L"QRSTUVWXYJKLMNOP";
    wchar_t path[64] = L"";
    for (const wchar_t *d = candidates; *d; d++) {
        wchar_t root[4] = { *d, L':', L'\\', 0 };
        if (GetFileAttributesW(root) == INVALID_FILE_ATTRIBUTES) {
            _snwprintf(path, 64, L"%lc:\\Odyssey 3D Hub", *d);
            break;
        }
    }
    if (!path[0]) SKIP("no free drive letter");
    kset_sz(c->hub, VALUE_INSTALL_PATH, path);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_FOREIGN_SDK);
    wchar_t got[TU_PATH];
    kget(c->hub, VALUE_INSTALL_PATH, got, TU_PATH);
    CHECK_EQ_WSTR(got, path);
}

TEST(auto_quoted_install_path_is_not_replaced)
{
    Ctx *c = ctx_new(L"quoted", 1);
    const wchar_t *v = L"\"C:\\Program Files\\Odyssey 3D Hub\"";
    kset_sz(c->hub, VALUE_INSTALL_PATH, v);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_FOREIGN_SDK);
}

TEST(auto_replaces_another_copy_of_the_mod_and_keeps_what_it_saved)
{
    Ctx *c = ctx_new(L"other", 1);
    wchar_t other[TU_PATH];
    sub_dir(other, c->tmp, L"old game\\bin\\x64\\Braindance3D", 1);
    put_sdk(other, SDK_OURS);
    kset_sz(c->hub, VALUE_INSTALL_PATH, other);
    kset_sz(c->hub, VALUE_PREVIOUS, L"D:\\Samsung Hub");
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    check_installed(c->hub, c->target, L"D:\\Samsung Hub");

    // Without a saved previous value there is nothing to restore later.
    RegDeleteTreeW(HKEY_CURRENT_USER, c->reg->root);
    kset_sz(c->hub, VALUE_INSTALL_PATH, other);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    check_installed(c->hub, c->target, NO_PREVIOUS);
}

TEST(auto_recognises_another_copy_by_its_settings_file)
{
    Ctx *c = ctx_new(L"othersettings", 1);
    wchar_t other[TU_PATH];
    sub_dir(other, c->tmp, L"older build", 1);
    put_sdk(other, SDK_FOREIGN_NO_VERSION);
    put_settings(other);
    kset_sz(c->hub, VALUE_INSTALL_PATH, other);
    kset_sz(c->hub, VALUE_PREVIOUS, NO_PREVIOUS);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    check_installed(c->hub, c->target, NO_PREVIOUS);
}

TEST(auto_recognises_its_own_earlier_path_after_that_folder_is_deleted)
{
    Ctx *c = ctx_new(L"ownpath", 1);
    wchar_t old[TU_PATH];
    sub_dir(old, c->tmp, L"deleted copy", 0);
    kset_sz(c->hub, VALUE_INSTALL_PATH, old);
    kset_sz(c->hub, VALUE_OWN_PATH, old);
    kset_sz(c->hub, VALUE_PREVIOUS, L"E:\\Odyssey 3D Hub");
    kset_sz(c->hub, VALUE_MARKER, L"0.0.9");
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    // The first install's saved value survives; the deleted copy is not "previous".
    check_installed(c->hub, c->target, L"E:\\Odyssey 3D Hub");
}

TEST(auto_never_replaces_a_foreign_sdk)
{
    Ctx *c = ctx_new(L"foreign", 1);
    int kinds[] = { SDK_FOREIGN_NO_VERSION, SDK_FOREIGN_PRODUCT };
    for (int i = 0; i < 2; i++) {
        wchar_t hub[TU_PATH];
        _snwprintf(hub, TU_PATH, L"%ls\\Hub %d", c->tmp, i);
        REQUIRE(tu_mkdirs(hub));
        put_sdk(hub, kinds[i]);
        RegDeleteTreeW(HKEY_CURRENT_USER, c->reg->root);
        kset_sz(c->hub, VALUE_INSTALL_PATH, hub);
        char before[8192], after[8192];
        kdump(c->hub, before, sizeof(before));
        CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_FOREIGN_SDK);
        CHECK_EQ_INT(fi_set_calls, 0);
        kdump(c->hub, after, sizeof(after));
        CHECK_EQ_STR(after, before);
    }
}

TEST(auto_foreign_sdk_is_kept_even_with_leftover_bookkeeping)
{
    // Uninstalled by hand (InstallPath put back to Samsung's) but the mod's values were left behind.
    Ctx *c = ctx_new(L"foreignleft", 1);
    wchar_t hub[TU_PATH];
    sub_dir(hub, c->tmp, L"Hub", 1);
    put_sdk(hub, SDK_FOREIGN_NO_VERSION);
    kset_sz(c->hub, VALUE_INSTALL_PATH, hub);
    kset_sz(c->hub, VALUE_OWN_PATH, c->target);
    kset_sz(c->hub, VALUE_PREVIOUS, NO_PREVIOUS);
    kset_sz(c->hub, VALUE_MARKER, L"" BD3D_VERSION_STR);
    char before[8192], after[8192];
    kdump(c->hub, before, sizeof(before));
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_FOREIGN_SDK);
    kdump(c->hub, after, sizeof(after));
    CHECK_EQ_STR(after, before);
}

TEST(auto_never_overrides_samsung_in_its_default_folder)
{
    Ctx *c = ctx_new(L"samsung", 1);
    REQUIRE(tu_mkdirs(c->samsung));
    put_sdk(c->samsung, SDK_FOREIGN_PRODUCT);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_SAMSUNG_DEFAULT);
    CHECK(!kexists(c->hub));
    CHECK_EQ_INT(fi_set_calls, 0);

    // A key with only leftovers in it is not touched either.
    kset_sz(c->hub, VALUE_PREVIOUS, NO_PREVIOUS);
    char before[8192], after[8192];
    kdump(c->hub, before, sizeof(before));
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_SAMSUNG_DEFAULT);
    kdump(c->hub, after, sizeof(after));
    CHECK_EQ_STR(after, before);
}

TEST(auto_ignores_the_samsung_folder_when_it_holds_no_samsung_sdk)
{
    Ctx *c = ctx_new(L"samsungno", 1);
    // Folder missing.
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    // Folder without a DLL.
    RegDeleteTreeW(HKEY_CURRENT_USER, c->reg->root);
    REQUIRE(tu_mkdirs(c->samsung));
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    // This mod copied into Samsung's folder is not Samsung's SDK.
    RegDeleteTreeW(HKEY_CURRENT_USER, c->reg->root);
    put_sdk(c->samsung, SDK_OURS);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    // No Samsung folder known.
    RegDeleteTreeW(HKEY_CURRENT_USER, c->reg->root);
    CHECK_EQ_INT(run_auto(c, c->target, L""), AUTO_INSTALLED);
    RegDeleteTreeW(HKEY_CURRENT_USER, c->reg->root);
    CHECK_EQ_INT(run_auto(c, c->target, NULL), AUTO_INSTALLED);
    check_installed(c->hub, c->target, NO_PREVIOUS);
}

TEST(auto_treats_an_empty_install_path_as_unset)
{
    Ctx *c = ctx_new(L"emptyval", 1);
    REQUIRE(tu_mkdirs(c->samsung));
    put_sdk(c->samsung, SDK_FOREIGN_NO_VERSION);
    kset_sz(c->hub, VALUE_INSTALL_PATH, L"");
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_SAMSUNG_DEFAULT);
    CHECK_EQ_INT(run_auto(c, c->target, L""), AUTO_INSTALLED);
    check_installed(c->hub, c->target, NO_PREVIOUS);
}

TEST(auto_writes_the_bookkeeping_before_install_path)
{
    Ctx *c = ctx_new(L"order", 1);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    REQUIRE_EQ_INT(fi_set_calls, 4);
    CHECK_EQ_WSTR(fi_names[3], VALUE_INSTALL_PATH);
    int seen_prev = 0, seen_own = 0, seen_marker = 0;
    for (int i = 0; i < 3; i++) {
        seen_prev |= !wcscmp(fi_names[i], VALUE_PREVIOUS);
        seen_own |= !wcscmp(fi_names[i], VALUE_OWN_PATH);
        seen_marker |= !wcscmp(fi_names[i], VALUE_MARKER);
    }
    CHECK(seen_prev && seen_own && seen_marker);
}

TEST(auto_failed_write_never_points_install_path_here)
{
    Ctx *c = ctx_new(L"failwrite", 1);
    wchar_t gone[TU_PATH];
    sub_dir(gone, c->tmp, L"old", 0);
    for (int k = 0; k < 4; k++) {
        RegDeleteTreeW(HKEY_CURRENT_USER, c->reg->root);
        kset_sz(c->hub, VALUE_INSTALL_PATH, gone);
        fi_fail_set_at = k;
        CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_ERROR);
        fi_fail_set_at = -1;
        check_value(c->hub, VALUE_INSTALL_PATH, gone);
        CHECK_MSG(fi_set_calls == k + 1, "write %d failed but %d writes were tried", k, fi_set_calls);
    }
    // When the bookkeeping got written, the next start completes the install with the same saved value.
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_INSTALLED);
    check_installed(c->hub, c->target, gone);
}

TEST(auto_key_that_cannot_be_created_is_an_error)
{
    Ctx *c = ctx_new(L"failcreate", 1);
    fi_fail_create = 1;
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_ERROR);
    fi_fail_create = 0;
    CHECK(!kexists(c->hub));
}

TEST(auto_handles_non_ascii_paths)
{
    Ctx *c = ctx_new(L"unicode", 0);
    wchar_t target[TU_PATH], gone[TU_PATH];
    sub_dir(target, c->tmp, L"Cyberpunk 2077 [GOG] " TU_NONASCII L"\\bin\\x64\\Braindance3D", 1);
    put_sdk(target, SDK_OURS);
    sub_dir(gone, c->tmp, L"Odyssey " TU_NONASCII, 0);
    kset_sz(c->hub, VALUE_INSTALL_PATH, gone);
    CHECK_EQ_INT(run_auto(c, target, c->samsung), AUTO_INSTALLED);
    check_installed(c->hub, target, gone);
    CHECK_EQ_INT(run_auto(c, target, c->samsung), AUTO_ALREADY);
}

TEST(auto_expandable_install_path_to_a_foreign_sdk_is_kept)
{
    Ctx *c = ctx_new(L"expand", 1);
    wchar_t hub[TU_PATH];
    sub_dir(hub, c->tmp, L"Hub", 1);
    put_sdk(hub, SDK_FOREIGN_PRODUCT);
    tu_setenv(L"BD3D_TEST_HUB_ROOT", c->tmp);
    const wchar_t *v = L"%BD3D_TEST_HUB_ROOT%\\Hub";
    kset(c->hub, VALUE_INSTALL_PATH, REG_EXPAND_SZ, v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_FOREIGN_SDK);
}

TEST(auto_install_path_of_max_path_or_more_characters_is_not_lost)
{
    Ctx *c = ctx_new(L"longval", 1);
    wchar_t lng[600];
    wcscpy(lng, L"C:\\");
    while (wcslen(lng) < 300) wcscat(lng, L"z");
    kset_sz(c->hub, VALUE_INSTALL_PATH, lng);
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_FOREIGN_SDK);
    wchar_t got[2048];
    kget(c->hub, VALUE_INSTALL_PATH, got, 2048);
    CHECK_MSG(wcscmp(got, lng) == 0, "the long InstallPath was changed");
}

TEST(auto_non_string_install_path_is_not_replaced)
{
    Ctx *c = ctx_new(L"dwordval", 1);
    DWORD dw = 7;
    kset(c->hub, VALUE_INSTALL_PATH, REG_DWORD, &dw, sizeof(dw));
    CHECK_EQ_INT(run_auto(c, c->target, c->samsung), AUTO_FOREIGN_SDK);
}

// ===================================================================================== black box: the built plugin

TEST(plugin_exports_exactly_query_supports_and_main)
{
    HMODULE h = plugin_load();
    IMAGE_NT_HEADERS64 *nt = tu_nt(h);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    REQUIRE(d.VirtualAddress != 0);
    IMAGE_EXPORT_DIRECTORY *e = (IMAGE_EXPORT_DIRECTORY *)((BYTE *)h + d.VirtualAddress);
    DWORD *names = (DWORD *)((BYTE *)h + e->AddressOfNames);
    int found = 0;
    for (DWORD i = 0; i < e->NumberOfNames; i++) {
        const char *n = (const char *)((BYTE *)h + names[i]);
        int known = !strcmp(n, "Query") || !strcmp(n, "Supports") || !strcmp(n, "Main");
        CHECK_MSG(known, "unexpected export %s", n);
        found += known;
    }
    CHECK_EQ_INT(found, 3);
    CHECK(tu_in_code(h, (void *)GetProcAddress(h, "Query")));
    CHECK(tu_in_code(h, (void *)GetProcAddress(h, "Supports")));
    CHECK(tu_in_code(h, (void *)GetProcAddress(h, "Main")));
}

TEST(plugin_pe_is_x64_dll_with_aslr_nx_and_high_entropy_va)
{
    HMODULE h = plugin_load();
    IMAGE_NT_HEADERS64 *nt = tu_nt(h);
    CHECK_EQ_INT(nt->FileHeader.Machine, IMAGE_FILE_MACHINE_AMD64);
    CHECK(nt->FileHeader.Characteristics & IMAGE_FILE_DLL);
    WORD dc = nt->OptionalHeader.DllCharacteristics;
    CHECK(dc & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
    CHECK(dc & IMAGE_DLLCHARACTERISTICS_NX_COMPAT);
    CHECK(dc & IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA);
}

TEST(plugin_imports_only_windows_system_dlls)
{
    HMODULE h = plugin_load();
    IMAGE_DATA_DIRECTORY dir = tu_nt(h)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    REQUIRE(dir.VirtualAddress != 0);
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)h + dir.VirtualAddress);
    static const char *allowed[] = { "kernel32.dll", "msvcrt.dll", "ntdll.dll", "advapi32.dll", "ucrtbase.dll",
                                     "version.dll" };
    int n = 0;
    for (; imp->Name; imp++, n++) {
        const char *name = (const char *)((BYTE *)h + imp->Name);
        int ok = !_strnicmp(name, "api-ms-win-", 11);
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
            if (!_stricmp(name, allowed[i])) ok = 1;
        CHECK_MSG(ok, "imports %s, which is not part of Windows", name);
    }
    CHECK(n > 0);
}

TEST(plugin_version_resource_names_the_product_and_the_file)
{
    REQUIRE(tu_exists(plugin_path()));
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(plugin_path(), &dummy);
    REQUIRE_MSG(size > 0, "no VERSIONINFO resource");
    void *buf = malloc(size);
    tt_defer(free, buf);
    REQUIRE(GetFileVersionInfoW(plugin_path(), 0, size, buf));
    VS_FIXEDFILEINFO *ffi = NULL;
    UINT len = 0;
    REQUIRE(VerQueryValueW(buf, L"\\", (void **)&ffi, &len) && ffi);
    CHECK_EQ_INT(HIWORD(ffi->dwFileVersionMS), BD3D_VERSION_MAJOR);
    CHECK_EQ_INT(LOWORD(ffi->dwFileVersionMS), BD3D_VERSION_MINOR);
    CHECK_EQ_INT(HIWORD(ffi->dwFileVersionLS), BD3D_VERSION_PATCH);
    struct { const wchar_t *key, *want; } kv[] = {
        { L"\\StringFileInfo\\040904b0\\ProductName", L"" BD3D_PRODUCT_NAME },
        { L"\\StringFileInfo\\040904b0\\ProductVersion", L"" BD3D_VERSION_STR },
        { L"\\StringFileInfo\\040904b0\\FileVersion", L"" BD3D_VERSION_STR },
        { L"\\StringFileInfo\\040904b0\\OriginalFilename", L"Braindance3D.dll" },
    };
    for (size_t i = 0; i < sizeof(kv) / sizeof(kv[0]); i++) {
        wchar_t *val = NULL;
        UINT n = 0;
        CHECK_MSG(VerQueryValueW(buf, kv[i].key, (void **)&val, &n) && val, "missing %ls", kv[i].key);
        if (val) CHECK_EQ_WSTR(val, kv[i].want);
    }
}

TEST(plugin_query_fills_the_56_byte_plugin_info)
{
    HMODULE h = plugin_load();
    PFN_Query query = (PFN_Query)(void *)GetProcAddress(h, "Query");
    REQUIRE(query);
    unsigned char buf[64];
    memset(buf, 0xCC, sizeof(buf));
    query(buf);
    uint8_t u8;
    uint16_t u16[4];
    uint32_t u32[2];
    const wchar_t *name, *author;
    // sdk: 0.5.0, no prerelease
    memcpy(&u8, buf + 0, 1);
    CHECK_EQ_INT(u8, 0);
    memcpy(u16, buf + 2, 2);
    CHECK_EQ_INT(u16[0], 5);
    memcpy(u32, buf + 4, 4);
    CHECK_EQ_INT(u32[0], 0);
    memcpy(u32, buf + 8, 8);
    CHECK_EQ_INT(u32[0], 0);
    CHECK_EQ_INT(u32[1], 0);
    // name, author
    memcpy(&name, buf + 16, sizeof(name));
    memcpy(&author, buf + 24, sizeof(author));
    REQUIRE(name && author);
    CHECK(tu_in_image(h, name));
    CHECK(tu_in_image(h, author));
    CHECK_MSG(wcsstr(name, L"" BD3D_PRODUCT_NAME) != NULL, "name '%ls'", name);
    CHECK(wcslen(author) > 0);
    // version: version.h, no prerelease
    memcpy(&u8, buf + 32, 1);
    CHECK_EQ_INT(u8, BD3D_VERSION_MAJOR);
    memcpy(u16, buf + 34, 2);
    CHECK_EQ_INT(u16[0], BD3D_VERSION_MINOR);
    memcpy(u32, buf + 36, 4);
    CHECK_EQ_INT(u32[0], BD3D_VERSION_PATCH);
    memcpy(u32, buf + 40, 8);
    CHECK_EQ_INT(u32[0], 0);
    CHECK_EQ_INT(u32[1], 0);
    // runtime: any game version
    memcpy(u16, buf + 48, 8);
    for (int i = 0; i < 4; i++) CHECK_EQ_INT(u16[i], 0xFFFF);
    // Nothing past the 56 bytes.
    for (int i = 56; i < 64; i++) CHECK_MSG(buf[i] == 0xCC, "byte %d past the struct was written", i);

    // The strings are static: a second call gives the same pointers.
    unsigned char buf2[64];
    memset(buf2, 0, sizeof(buf2));
    query(buf2);
    CHECK(memcmp(buf + 16, buf2 + 16, 16) == 0);
}

TEST(plugin_supports_reports_api_v0)
{
    HMODULE h = plugin_load();
    PFN_Supports supports = (PFN_Supports)(void *)GetProcAddress(h, "Supports");
    REQUIRE(supports);
    CHECK_EQ_INT(supports(), 0);
}

TEST(plugin_loading_and_unloading_has_no_side_effects)
{
    guard_real_samsung();
    wchar_t *lad = tu_tempdir(L"loadlad");
    tu_setenv(L"LOCALAPPDATA", lad);
    for (int i = 0; i < 3; i++) {
        HMODULE h = LoadLibraryExW(plugin_path(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        REQUIRE(h);
        FreeLibrary(h);
    }
    wchar_t sub[TU_PATH];
    tu_join(sub, TU_PATH, lad, L"Braindance3D");
    CHECK(!tu_exists(sub));
}

// ===================================================================================== black box: Main in a game

typedef struct {
    wchar_t *root;
    Scratch *reg;
    wchar_t game[TU_PATH], bin[TU_PATH], exe[TU_PATH], target[TU_PATH], plugin_dir[TU_PATH], plugin[TU_PATH];
    wchar_t pf[TU_PATH], samsung[TU_PATH], lad[TU_PATH], log[TU_PATH], lad_log[TU_PATH];
    wchar_t hub[512]; // HKCU-relative path of the key Main writes (under the scratch key)
} Game;

// A game folder: <root>\<name>\bin\x64\Cyberpunk2077.exe (a copy of this program), the plugin in
// red4ext\plugins\Braindance3D, and (with_sdk) the mod in bin\x64\Braindance3D. ProgramFiles and LOCALAPPDATA
// point into <root> for the child process.
static Game *game_new(const wchar_t *tag, const wchar_t *name, int with_sdk)
{
    REQUIRE_MSG(tu_exists(plugin_path()), "plugin under test not found: %ls (build it with ./build.sh, or pass --plugin)",
                plugin_path());
    guard_real_samsung();
    Game *g = calloc(1, sizeof(*g));
    tt_defer(free, g);
    g->root = tu_tempdir(tag);
    sub_dir(g->game, g->root, name, 1);
    sub_dir(g->bin, g->game, L"bin\\x64", 1);
    tu_join(g->exe, TU_PATH, g->bin, L"Cyberpunk2077.exe");
    wchar_t self[TU_PATH];
    GetModuleFileNameW(NULL, self, TU_PATH);
    copy_file(self, g->exe);
    sub_dir(g->plugin_dir, g->game, L"red4ext\\plugins\\Braindance3D", 1);
    tu_join(g->plugin, TU_PATH, g->plugin_dir, L"Braindance3D.dll");
    copy_file(plugin_path(), g->plugin);
    sub_dir(g->target, g->bin, L"Braindance3D", 0);
    if (with_sdk) {
        REQUIRE(tu_mkdirs(g->target));
        put_sdk(g->target, SDK_OURS);
        put_settings(g->target);
    }
    tu_join(g->log, TU_PATH, g->target, LOG_NAME);
    sub_dir(g->pf, g->root, L"Program Files", 1);
    sub_dir(g->samsung, g->pf, L"Odyssey 3D Hub", 0);
    sub_dir(g->lad, g->root, L"LocalAppData", 1);
    _snwprintf(g->lad_log, TU_PATH, L"%ls\\Braindance3D\\" LOG_NAME, g->lad);
    g->reg = scratch_new(L"SOFTWARE\\Samsung\\Odyssey3DHub");
    wcscpy(g->hub, g->reg->hub);
    tu_setenv(L"BD3D_CHILD_PLUGIN", g->plugin);
    tu_setenv(L"BD3D_CHILD_KEY", g->reg->root);
    tu_setenv(L"ProgramFiles", g->pf);
    tu_setenv(L"LOCALAPPDATA", g->lad);
    return g;
}

// Runs Main(reason) in the game's exe. Returns the child's exit code: 0 when Main returned 1.
static DWORD game_run(Game *g, int reason)
{
    wchar_t cmd[TU_PATH * 2], r[16];
    _snwprintf(r, 16, L"%d", reason);
    SetEnvironmentVariableW(L"BD3D_CHILD_REASON", r);
    _snwprintf(cmd, TU_PATH * 2, L"\"%ls\" --r4e-main", g->exe);
    cmd[TU_PATH * 2 - 1] = 0;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    REQUIRE_MSG(CreateProcessW(g->exe, cmd, NULL, NULL, FALSE, 0, NULL, g->bin, &si, &pi), "CreateProcess failed (%lu)",
                GetLastError());
    DWORD w = WaitForSingleObject(pi.hProcess, 60000);
    if (w != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 99);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    SetEnvironmentVariableW(L"BD3D_CHILD_REASON", NULL);
    REQUIRE_MSG(w == WAIT_OBJECT_0, "Main did not return within 60 s");
    if (code == 29) SKIP("this system resets %%ProgramFiles%% in new processes (Wine does), so Main can't be pointed at a test Samsung folder");
    REQUIRE_MSG(code < 20 || code >= 30, "the child could not set up a scratch HKCU (code %lu): Main was not run", code);
    return code;
}

// The child: HKCU -> the scratch key, verified, then Main() as RED4ext calls it.
static int r4e_child(void)
{
    wchar_t plugin[TU_PATH], key[512], reason[16];
    if (!GetEnvironmentVariableW(L"BD3D_CHILD_PLUGIN", plugin, TU_PATH)) return 20;
    if (!GetEnvironmentVariableW(L"BD3D_CHILD_KEY", key, 512) || !scratch_is_safe(key)) return 21;
    if (!GetEnvironmentVariableW(L"BD3D_CHILD_REASON", reason, 16)) return 22;
    // Tests of Samsung's default folder need the ProgramFiles the parent set.
    wchar_t want[TU_PATH], pf[TU_PATH];
    if (GetEnvironmentVariableW(L"BD3D_CHILD_PROGRAMFILES", want, TU_PATH) &&
        (!GetEnvironmentVariableW(L"ProgramFiles", pf, TU_PATH) || _wcsicmp(pf, want) != 0))
        return 29;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_ALL_ACCESS, &k) != ERROR_SUCCESS) return 23;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, k) != ERROR_SUCCESS) return 24;
    HKEY canary;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"bd3d-canary", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &canary, NULL) != ERROR_SUCCESS)
        return 25;
    RegCloseKey(canary);
    if (RegOpenKeyExW(k, L"bd3d-canary", 0, KEY_READ, &canary) != ERROR_SUCCESS) return 26; // not redirected
    RegCloseKey(canary);
    RegDeleteKeyW(k, L"bd3d-canary");
    HMODULE h = LoadLibraryExW(plugin, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) return 27;
    PFN_Main m = (PFN_Main)(void *)GetProcAddress(h, "Main");
    if (!m) return 28;
    int r = m(h, (uint8_t)_wtoi(reason), NULL);
    FreeLibrary(h);
    return r == 1 ? 0 : 30;
}

TEST(plugin_main_ignores_reasons_other_than_load)
{
    Game *g = game_new(L"main-unload", L"Cyberpunk 2077", 1);
    int reasons[] = { 1, 2, 255 };
    for (int i = 0; i < 3; i++) CHECK_EQ_INT(game_run(g, reasons[i]), 0);
    CHECK(!kexists(g->hub));
    CHECK(!tu_exists(g->log));
    CHECK(!tu_exists(g->lad_log));
}

TEST(plugin_main_registers_a_fresh_game)
{
    Game *g = game_new(L"main-fresh", L"Cyberpunk 2077", 1);
    CHECK_EQ_INT(game_run(g, 0), 0);
    check_installed(g->hub, g->target, NO_PREVIOUS);
    CHECK(tu_file_size(g->log) > 0);
    CHECK(!tu_exists(g->lad_log));
    // Nothing is written next to the plugin.
    WIN32_FIND_DATAW fd;
    wchar_t pattern[TU_PATH];
    tu_join(pattern, TU_PATH, g->plugin_dir, L"*");
    HANDLE f = FindFirstFileW(pattern, &fd);
    REQUIRE(f != INVALID_HANDLE_VALUE);
    int files = 0;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
            files++;
            CHECK_EQ_WSTR(fd.cFileName, L"Braindance3D.dll");
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    CHECK_EQ_INT(files, 1);
}

TEST(plugin_main_on_later_starts_changes_nothing)
{
    Game *g = game_new(L"main-again", L"Cyberpunk 2077", 1);
    CHECK_EQ_INT(game_run(g, 0), 0);
    kset_sz(g->hub, VALUE_MARKER, L"sentinel");
    char before[8192], after[8192];
    kdump(g->hub, before, sizeof(before));
    long long size = tu_file_size(g->log);
    CHECK_EQ_INT(game_run(g, 0), 0);
    kdump(g->hub, after, sizeof(after));
    CHECK_EQ_STR(after, before);
    CHECK_MSG(tu_file_size(g->log) > size, "the second start did not append to the log");
}

TEST(plugin_main_keeps_a_registered_foreign_sdk)
{
    Game *g = game_new(L"main-foreign", L"Cyberpunk 2077", 1);
    REQUIRE(tu_mkdirs(g->samsung));
    put_sdk(g->samsung, SDK_FOREIGN_PRODUCT);
    kset_sz(g->hub, VALUE_INSTALL_PATH, g->samsung);
    char before[8192], after[8192];
    kdump(g->hub, before, sizeof(before));
    CHECK_EQ_INT(game_run(g, 0), 0);
    kdump(g->hub, after, sizeof(after));
    CHECK_EQ_STR(after, before);
}

TEST(plugin_main_keeps_samsung_in_its_default_folder)
{
    Game *g = game_new(L"main-samsung", L"Cyberpunk 2077", 1);
    tu_setenv(L"BD3D_CHILD_PROGRAMFILES", g->pf);
    REQUIRE(tu_mkdirs(g->samsung));
    put_sdk(g->samsung, SDK_FOREIGN_NO_VERSION);
    CHECK_EQ_INT(game_run(g, 0), 0);
    CHECK(!kexists(g->hub));
    CHECK(tu_file_size(g->log) > 0);
}

TEST(plugin_main_replaces_another_copy_of_the_mod)
{
    Game *g = game_new(L"main-other", L"Cyberpunk 2077", 1);
    wchar_t other[TU_PATH];
    sub_dir(other, g->root, L"Old Game\\bin\\x64\\Braindance3D", 1);
    put_sdk(other, SDK_OURS);
    kset_sz(g->hub, VALUE_INSTALL_PATH, other);
    kset_sz(g->hub, VALUE_OWN_PATH, other);
    kset_sz(g->hub, VALUE_PREVIOUS, L"D:\\Samsung\\Odyssey 3D Hub");
    CHECK_EQ_INT(game_run(g, 0), 0);
    check_installed(g->hub, g->target, L"D:\\Samsung\\Odyssey 3D Hub");
}

TEST(plugin_main_saves_a_dead_install_path_for_uninstall)
{
    Game *g = game_new(L"main-dead", L"Cyberpunk 2077", 1);
    wchar_t gone[TU_PATH];
    sub_dir(gone, g->root, L"uninstalled hub", 0);
    kset_sz(g->hub, VALUE_INSTALL_PATH, gone);
    CHECK_EQ_INT(game_run(g, 0), 0);
    check_installed(g->hub, g->target, gone);
}

TEST(plugin_main_without_the_sdk_changes_nothing_and_logs_to_localappdata)
{
    Game *g = game_new(L"main-nosdk", L"Cyberpunk 2077", 0);
    CHECK_EQ_INT(game_run(g, 0), 0);
    CHECK(!kexists(g->hub));
    CHECK(!tu_exists(g->target));
    CHECK(tu_file_size(g->lad_log) > 0);
}

TEST(plugin_main_log_starts_over_past_256_kb)
{
    Game *g = game_new(L"main-biglog", L"Cyberpunk 2077", 1);
    size_t big = 300 * 1024;
    char *junk = malloc(big);
    memset(junk, 'x', big);
    REQUIRE(tu_write_file(g->log, junk, big));
    free(junk);
    CHECK_EQ_INT(game_run(g, 0), 0);
    long long size = tu_file_size(g->log);
    CHECK_MSG(size > 0 && size < 64 * 1024, "log is %lld bytes", size);
    check_installed(g->hub, g->target, NO_PREVIOUS);
}

TEST(plugin_main_in_a_game_folder_with_spaces_brackets_and_non_ascii)
{
    Game *g = game_new(L"main-unicode", L"Cyberpunk 2077 [GOG] (1) " TU_NONASCII, 1);
    CHECK_EQ_INT(game_run(g, 0), 0);
    check_installed(g->hub, g->target, NO_PREVIOUS);
    CHECK(tu_file_size(g->log) > 0);
    CHECK_EQ_INT(game_run(g, 0), 0);
    check_installed(g->hub, g->target, NO_PREVIOUS);
}

// ===================================================================================== main

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--r4e-main")) return r4e_child();
    tt_set_args(argc, argv);
    // Keep any fallback log out of the real %LOCALAPPDATA%.
    wchar_t tmp[TU_PATH], lad[TU_PATH];
    GetTempPathW(TU_PATH, tmp);
    _snwprintf(lad, TU_PATH, L"%lsbd3d-tests-%lu-localappdata", tmp, GetCurrentProcessId());
    lad[TU_PATH - 1] = 0;
    CreateDirectoryW(lad, NULL);
    SetEnvironmentVariableW(L"LOCALAPPDATA", lad);
    char p1[TU_PATH * 3], p2[TU_PATH * 3];
    tu_utf8(plugin_path(), p1, sizeof(p1));
    tu_utf8(tu_dll_path(), p2, sizeof(p2));
    printf("Plugin under test: %s\nSDK DLL used as this mod's SDK: %s%s\n", p1, p2,
           tu_running_under_wine() ? " (running under Wine)" : "");
    int rc = tt_main(argc, argv);
    tu_rmrf(lad);
    return rc;
}
