// testutil.h - helpers shared by the native tests: temp folders, files, the DLL under test,
// PE inspection and heap accounting. Header-only (static functions).

#ifndef BD3D_TESTUTIL_H
#define BD3D_TESTUTIL_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "tinytest.h"

#define TU_PATH 1024

// A string with characters that need Unicode paths: a-umlaut, a snowman and "test" in katakana.
#define TU_NONASCII L"Bräindance ☃ テスト"

static int tu_exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static int tu_is_dir(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void tu_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b)
{
    _snwprintf(out, cap, L"%ls\\%ls", a, b);
    out[cap - 1] = 0;
}

// Creates every missing folder of p (absolute path).
static int tu_mkdirs(const wchar_t *p)
{
    wchar_t buf[TU_PATH];
    wcsncpy(buf, p, TU_PATH - 1);
    buf[TU_PATH - 1] = 0;
    for (wchar_t *s = buf + 3; *s; s++) {
        if (*s == L'\\' || *s == L'/') {
            wchar_t c = *s;
            *s = 0;
            CreateDirectoryW(buf, NULL);
            *s = c;
        }
    }
    CreateDirectoryW(buf, NULL);
    return tu_is_dir(p);
}

// Deletes a file or a folder tree. Clears read-only attributes on the way.
static void tu_rmrf(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p);
        return;
    }
    wchar_t pat[TU_PATH];
    tu_join(pat, TU_PATH, p, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t child[TU_PATH];
            tu_join(child, TU_PATH, p, fd.cFileName);
            tu_rmrf(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(p);
}

static void tu_rmrf_deferred(void *p)
{
    tu_rmrf((const wchar_t *)p);
    free(p);
}

// Makes a fresh, empty temp folder "<TEMP>\bd3d-tests-<pid>-<n>-<tag>" and deletes it after the test.
static wchar_t *tu_tempdir(const wchar_t *tag)
{
    static LONG counter;
    wchar_t base[TU_PATH];
    DWORD n = GetTempPathW(TU_PATH, base);
    if (n == 0 || n >= TU_PATH) wcscpy(base, L".\\");
    size_t len = wcslen(base);
    if (len && (base[len - 1] == L'\\' || base[len - 1] == L'/')) base[len - 1] = 0;
    wchar_t *dir = calloc(TU_PATH, sizeof(wchar_t));
    _snwprintf(dir, TU_PATH, L"%ls\\bd3d-tests-%lu-%ld-%ls", base, GetCurrentProcessId(),
               InterlockedIncrement(&counter), tag ? tag : L"t");
    tu_rmrf(dir);
    REQUIRE_MSG(tu_mkdirs(dir), "cannot create temp folder %ls", dir);
    tt_defer(tu_rmrf_deferred, dir);
    return dir;
}

static int tu_write_file(const wchar_t *path, const void *data, size_t size)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data, (DWORD)size, &w, NULL);
    CloseHandle(h);
    return ok && w == size;
}

static int tu_write_text(const wchar_t *path, const char *text) { return tu_write_file(path, text, strlen(text)); }

// Reads a whole file into a NUL-terminated buffer (free() it). Returns NULL when it can't.
static char *tu_read_file(const wchar_t *path, size_t *size_out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        return NULL;
    }
    char *buf = malloc((size_t)sz.QuadPart + 1);
    DWORD r = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)sz.QuadPart, &r, NULL);
    CloseHandle(h);
    if (!ok) {
        free(buf);
        return NULL;
    }
    buf[r] = 0;
    if (size_out) *size_out = r;
    return buf;
}

static long long tu_file_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

static int tu_count_lines(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (*s == '\n') n++;
    return n;
}

static void tu_utf8(const wchar_t *w, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
}

// ---------------------------------------------------------------- the DLL under test

// The built DLL: --dll <path>, else %BD3D_DLL%, else ..\Odyssey3DSDK.dll next to this exe
// (build\tests\x.exe -> build\Odyssey3DSDK.dll), else build\Odyssey3DSDK.dll in the current folder.
static const wchar_t *tu_dll_path(void)
{
    static wchar_t path[TU_PATH];
    if (path[0]) return path;
    wchar_t cand[TU_PATH] = L"";
    const char *opt = tt_option("dll");
    if (opt) {
        MultiByteToWideChar(CP_ACP, 0, opt, -1, cand, TU_PATH);
    } else if (GetEnvironmentVariableW(L"BD3D_DLL", cand, TU_PATH) == 0) {
        wchar_t exe[TU_PATH];
        GetModuleFileNameW(NULL, exe, TU_PATH);
        wchar_t *s = wcsrchr(exe, L'\\');
        if (s) *s = 0;
        _snwprintf(cand, TU_PATH, L"%ls\\..\\Odyssey3DSDK.dll", exe);
        cand[TU_PATH - 1] = 0;
        if (!tu_exists(cand)) wcscpy(cand, L"build\\Odyssey3DSDK.dll");
    }
    if (!GetFullPathNameW(cand, TU_PATH, path, NULL)) wcscpy(path, cand);
    return path;
}

static void tu_require_dll(void)
{
    REQUIRE_MSG(tu_exists(tu_dll_path()), "DLL under test not found: %ls (build it with ./build.sh, or pass --dll)",
                tu_dll_path());
}

// ---------------------------------------------------------------- PE inspection

static IMAGE_NT_HEADERS64 *tu_nt(HMODULE m)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    return (IMAGE_NT_HEADERS64 *)((BYTE *)m + dos->e_lfanew);
}

// 1 when p points into an executable section of module m.
static int tu_in_code(HMODULE m, const void *p)
{
    if (!m || !p) return 0;
    IMAGE_NT_HEADERS64 *nt = tu_nt(m);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    uintptr_t base = (uintptr_t)m, a = (uintptr_t)p;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t lo = base + s->VirtualAddress, hi = lo + s->Misc.VirtualSize;
        if (a >= lo && a < hi) return 1;
    }
    return 0;
}

// 1 when p points anywhere inside module m's image.
static int tu_in_image(HMODULE m, const void *p)
{
    if (!m || !p) return 0;
    uintptr_t base = (uintptr_t)m, a = (uintptr_t)p;
    return a >= base && a < base + tu_nt(m)->OptionalHeader.SizeOfImage;
}

// ---------------------------------------------------------------- heap accounting

// Bytes in busy blocks of the process heap. Used to detect leaks of objects the DLL allocates there.
static long long tu_heap_busy_bytes(void)
{
    HANDLE heap = GetProcessHeap();
    long long total = 0;
    PROCESS_HEAP_ENTRY e;
    e.lpData = NULL;
    if (!HeapLock(heap)) return -1;
    while (HeapWalk(heap, &e))
        if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) total += e.cbData;
    HeapUnlock(heap);
    return total;
}

// ---------------------------------------------------------------- environment

typedef struct { wchar_t name[64]; wchar_t *value; int had; } tu_saved_env;

static void tu_restore_env(void *p)
{
    tu_saved_env *s = p;
    SetEnvironmentVariableW(s->name, s->had ? s->value : NULL);
    free(s->value);
    free(s);
}

// Sets (or with value NULL, removes) an environment variable for the rest of the test.
static void tu_setenv(const wchar_t *name, const wchar_t *value)
{
    tu_saved_env *s = calloc(1, sizeof(*s));
    wcsncpy(s->name, name, 63);
    s->value = calloc(32768, sizeof(wchar_t));
    s->had = GetEnvironmentVariableW(name, s->value, 32768) > 0;
    SetEnvironmentVariableW(name, value);
    tt_defer(tu_restore_env, s);
}

static int tu_running_under_wine(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL;
}

// ---------------------------------------------------------------- windows

static LRESULT CALLBACK tu_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static void tu_destroy_window(void *h) { DestroyWindow((HWND)h); }

// A visible top-level window like the game's. Destroyed after the test.
static HWND tu_window(const wchar_t *title, int x, int y, int w, int h)
{
    static ATOM cls;
    if (!cls) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = tu_wndproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"bd3d-test-window";
        cls = RegisterClassW(&wc);
    }
    HWND hwnd = CreateWindowExW(0, L"bd3d-test-window", title, WS_POPUP | WS_VISIBLE, x, y, w, h, NULL, NULL,
                                GetModuleHandleW(NULL), NULL);
    if (!hwnd && tu_running_under_wine())
        SKIP("Wine has no display driver: run under Xvfb (tests/run-native-tests.sh does this when xvfb-run exists)");
    REQUIRE_MSG(hwnd != NULL, "CreateWindowEx failed (%lu)", GetLastError());
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    tt_defer(tu_destroy_window, hwnd);
    return hwnd;
}

static void tu_pump(void)
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

#endif // BD3D_TESTUTIL_H
