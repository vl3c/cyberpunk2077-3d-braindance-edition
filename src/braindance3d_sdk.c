// 3D Braindance Edition
// Stand-in for Samsung's Odyssey3DSDK.dll, so Cyberpunk 2077 2.5 unlocks its native 3D Mode
// on an ordinary monitor. Paired with an engine ini (SBSDebug = true), the game then draws its own
// stereo render side by side, which a VR headset or 3D display can show in half-SBS mode.
//
// How the game finds the SDK:
//   HKCU\SOFTWARE\Samsung\Odyssey3DHub, value InstallPath
//   -> <InstallPath>\GameLauncherData\core\Odyssey3DSDK.dll
//   -> create_odyssey3d_factory_instance() / destroy_odyssey3d_factory_instance(factory*)
//
// The interface below was reverse-engineered from Cyberpunk2077.exe 2.5 (3.0.88.46235) call sites.
// No Samsung code is used. Slots marked "unmapped" are never seen called; they log and return
// their first argument, which is safe for the out-param getters this interface uses.
//
// The weaver (the part that would interlace for the Odyssey lenticular panel) is a logging
// no-op: in SBSDebug mode the game draws side by side itself and never creates one.
//
// License: MIT (see LICENSE).

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

#include "version.h"

#define EXPORT __declspec(dllexport)
#define NSLOTS 32

// ---------------------------------------------------------------- settings

// Braindance3D-Settings.ini in the install folder (two levels above the DLL's folder):
//   [Log]     Level = 0 off, 1 normal (default), 2 verbose
//   [Display] Monitor = auto (default: the monitor the game window is on) or primary
#define SETTINGS_FILE L"Braindance3D-Settings.ini"
static int g_logLevel = 1;
static int g_monitorPrimaryOnly = 0;

// Per-slot logging budget, set from the level.
static LONG64 g_fullLogCalls = 10;      // calls logged in full per slot
static double g_summaryInterval = 60.0; // seconds between per-slot call-count summaries

// ---------------------------------------------------------------- logging

static CRITICAL_SECTION g_logLock;
static FILE *g_log;
static LARGE_INTEGER g_freq, g_t0;
static HMODULE g_module;
// Paths are wide so non-ASCII folder names work; they are logged as UTF-8.
static wchar_t g_installDir[MAX_PATH]; // folder holding GameLauncherData\core (the registry InstallPath)
static wchar_t g_logPath[MAX_PATH];

// Converts a wide path for logging. Returns buf.
static const char *utf8(const wchar_t *w, char *buf, int size)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, size, NULL, NULL)) buf[0] = 0;
    return buf;
}

static double now_seconds(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart;
}

static void slog(const char *fmt, ...)
{
    if (!g_log) return;
    double t = now_seconds();
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_logLock);
    if (g_log) {
        fprintf(g_log, "%02d:%02d:%02d.%03d +%9.3fs [t%5lu] ", st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds, t, GetCurrentThreadId());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logLock);
}

// Strips the last path component in place. Returns 0 when there is none.
static int strip_last(wchar_t *path)
{
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    *slash = 0;
    return 1;
}

// Finds the install folder: <InstallPath>\GameLauncherData\core\Odyssey3DSDK.dll -> <InstallPath>.
static void find_install_dir(void)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { wcscpy(g_installDir, L"."); return; }
    strip_last(path); // ...\GameLauncherData\core
    wchar_t dllDir[MAX_PATH];
    wcscpy(dllDir, path);
    if (strip_last(path) && strip_last(path)) wcscpy(g_installDir, path);
    else wcscpy(g_installDir, dllDir);
}

// Parses a log level. A value that doesn't start with a digit means the default, 1.
static int parse_level(const wchar_t *v)
{
    while (*v == L' ' || *v == L'\t') v++;
    if (*v < L'0' || *v > L'9') return 1;
    long n = wcstol(v, NULL, 10);
    return n <= 0 ? 0 : n >= 2 ? 2 : (int)n;
}

static void read_settings(void)
{
    wchar_t ini[MAX_PATH], v[64];
    int n = _snwprintf(ini, MAX_PATH, L"%ls\\" SETTINGS_FILE, g_installDir);
    if (n < 0 || n >= MAX_PATH) return;
    GetPrivateProfileStringW(L"Log", L"Level", L"1", v, 64, ini);
    g_logLevel = parse_level(v);
    GetPrivateProfileStringW(L"Display", L"Monitor", L"auto", v, 64, ini);
    g_monitorPrimaryOnly = _wcsnicmp(v, L"primary", 7) == 0 && (!v[7] || wcschr(L" \t;#", v[7]));
    if (g_logLevel >= 2) { g_fullLogCalls = 120; g_summaryInterval = 1.0; }
    else { g_fullLogCalls = 10; g_summaryInterval = 60.0; }
}

// Opens <dir>\braindance3d.log, keeping the previous session as braindance3d.old.log.
// If the log is still open in another process (a game instance that is still exiting),
// it is left alone and this session logs to braindance3d-<pid>.log instead.
static FILE *open_log_in(const wchar_t *dir)
{
    wchar_t path[MAX_PATH], old[MAX_PATH];
    int n1 = _snwprintf(path, MAX_PATH, L"%ls\\braindance3d.log", dir);
    int n2 = _snwprintf(old, MAX_PATH, L"%ls\\braindance3d.old.log", dir);
    if (n1 < 0 || n1 >= MAX_PATH || n2 < 0 || n2 >= MAX_PATH) return NULL;
    if (!MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        n1 = _snwprintf(path, MAX_PATH, L"%ls\\braindance3d-%lu.log", dir, GetCurrentProcessId());
        if (n1 < 0 || n1 >= MAX_PATH) return NULL;
    }
    FILE *f = _wfopen(path, L"w");
    if (f) wcscpy(g_logPath, path);
    return f;
}

static void log_open(void)
{
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    if (g_logLevel <= 0) return;
    g_log = open_log_in(g_installDir);
    if (!g_log) {
        // The game folder may not be writable (Program Files): fall back to %LOCALAPPDATA%\Braindance3D.
        wchar_t dir[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 40) {
            wcscat(dir, L"\\Braindance3D");
            CreateDirectoryW(dir, NULL);
            g_log = open_log_in(dir);
        }
    }
}

// Full log for the first g_fullLogCalls calls of a slot, then one call-count summary per interval.
typedef struct { volatile LONG64 calls; LONG64 lastSummaryCalls; double lastSummaryTime; } SlotStats;

// Returns 1 when this call should be logged in full; logs a periodic summary otherwise.
static int should_log(SlotStats *s, const char *name)
{
    LONG64 n = InterlockedIncrement64(&s->calls);
    if (!g_log) return 0;
    if (n <= g_fullLogCalls) {
        if (n == g_fullLogCalls)
            slog("%s: reached %lld calls, now logging a count every %.0fs", name, (long long)n, g_summaryInterval);
        return 1;
    }
    double t = now_seconds();
    if (t - s->lastSummaryTime >= g_summaryInterval) {
        EnterCriticalSection(&g_logLock);
        int due = t - s->lastSummaryTime >= g_summaryInterval;
        LONG64 since = n - s->lastSummaryCalls;
        double span = t - s->lastSummaryTime;
        if (due) { s->lastSummaryCalls = n; s->lastSummaryTime = t; }
        LeaveCriticalSection(&g_logLock);
        if (due) slog("%s: %lld calls total (+%lld in last %.1fs)", name, (long long)n, (long long)since, span);
    }
    return 0;
}

static const char *dxgi_format_name(int f)
{
    switch (f) {
    case 2: return "R32G32B32A32_FLOAT";
    case 10: return "R16G16B16A16_FLOAT";
    case 24: return "R10G10B10A2_UNORM";
    case 26: return "R11G11B10_FLOAT";
    case 27: return "R8G8B8A8_TYPELESS";
    case 28: return "R8G8B8A8_UNORM";
    case 29: return "R8G8B8A8_UNORM_SRGB";
    case 87: return "B8G8R8A8_UNORM";
    case 91: return "B8G8R8A8_UNORM_SRGB";
    default: return "?";
    }
}

static void describe_resource(const char *what, ID3D12Resource *res)
{
    if (!res) { slog("  %s = NULL", what); return; }
    D3D12_RESOURCE_DESC d;
    res->lpVtbl->GetDesc(res, &d);
    slog("  %s = %p dim=%d %llux%u array=%u mips=%u format=%d(%s) samples=%u flags=0x%x",
         what, (void *)res, d.Dimension, (unsigned long long)d.Width, d.Height, d.DepthOrArraySize,
         d.MipLevels, d.Format, dxgi_format_name(d.Format), d.SampleDesc.Count, d.Flags);
}

// ---------------------------------------------------------------- monitor info
//
// The "3D monitor" is the monitor the game window is on, so 3D Mode can work on any monitor of
// a multi-monitor setup ([Display] Monitor = primary in the settings file turns this off).
// Before the window exists, the primary monitor is used.
//
// The game window is found once and kept while it stays valid and visible. GetMonitorRect
// (a handful of calls per session, at startup and on display changes) always reads the current
// monitor; GetResolution (~55 calls/s) reuses a result up to 0.5 s old. Nothing here sends
// window messages, so a busy game window can't block the caller.

#define MONITOR_REFRESH_S 0.5

static CRITICAL_SECTION g_monLock;
static RECT g_monitorRect = {0, 0, 1920, 1080};
static double g_monitorTime = -1.0, g_windowSearchTime = -1.0;
static HWND g_gameWnd;
static HMONITOR g_lastMonitor;
static RECT g_lastLoggedRect;

// A window that can be the game's main window: ours, visible, top-level, not a tool, overlay or console window.
static int is_candidate_window(HWND hwnd, DWORD pid)
{
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner != pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return 0;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return 0;
    if ((ex & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) == (WS_EX_LAYERED | WS_EX_TRANSPARENT)) return 0;
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"ConsoleWindowClass") == 0) return 0;
    RECT r;
    if (!GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) return 0;
    return 1;
}

typedef struct { DWORD pid; HWND best; LONG64 bestArea; } FindWindowCtx;

static BOOL CALLBACK find_game_window_cb(HWND hwnd, LPARAM lp)
{
    FindWindowCtx *ctx = (FindWindowCtx *)lp;
    if (!is_candidate_window(hwnd, ctx->pid)) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    LONG64 area = (LONG64)(r.right - r.left) * (r.bottom - r.top);
    if (area > ctx->bestArea) { ctx->best = hwnd; ctx->bestArea = area; }
    return TRUE;
}

static void log_window(const char *what, HWND hwnd)
{
    wchar_t cls[64] = L"";
    char clsUtf8[200];
    RECT r = {0};
    GetClassNameW(hwnd, cls, 64);
    GetWindowRect(hwnd, &r);
    slog("%s: hwnd=%p class='%s' rect %ld,%ld-%ld,%ld (%ldx%ld)", what, (void *)hwnd,
         utf8(cls, clsUtf8, sizeof(clsUtf8)), r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
}

// Keeps g_gameWnd pointing at the game window. Prefers the foreground window when it is ours.
// Searches at most once per MONITOR_REFRESH_S while there is no window.
static void update_game_window(double t)
{
    DWORD pid = GetCurrentProcessId();
    if (g_gameWnd && IsWindow(g_gameWnd) && is_candidate_window(g_gameWnd, pid)) return;
    if (g_gameWnd) {
        slog("game window %p is gone or hidden, looking again", (void *)g_gameWnd);
        g_gameWnd = NULL;
        g_windowSearchTime = -1.0;
    }
    if (g_windowSearchTime >= 0 && t - g_windowSearchTime < MONITOR_REFRESH_S) return;
    g_windowSearchTime = t;
    HWND fg = GetForegroundWindow();
    if (fg && is_candidate_window(fg, pid)) {
        g_gameWnd = fg;
    } else {
        FindWindowCtx ctx = { pid, NULL, 0 };
        EnumWindows(find_game_window_cb, (LPARAM)&ctx);
        g_gameWnd = ctx.best;
    }
    if (g_gameWnd) log_window("game window found", g_gameWnd);
}

static void refresh_monitor(double t)
{
    HMONITOR mon = NULL;
    if (!g_monitorPrimaryOnly) {
        update_game_window(t);
        if (g_gameWnd) mon = MonitorFromWindow(g_gameWnd, MONITOR_DEFAULTTONEAREST);
    }
    if (!mon) {
        POINT origin = {0, 0};
        mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, (MONITORINFO *)&mi)) {
        g_monitorRect = mi.rcMonitor;
        if (mon != g_lastMonitor || !EqualRect(&g_lastLoggedRect, &mi.rcMonitor)) {
            char dev[100];
            slog("3D monitor = %s (%s) rect l=%ld t=%ld r=%ld b=%ld (%ldx%ld)", utf8(mi.szDevice, dev, sizeof(dev)),
                 g_monitorPrimaryOnly ? "primary, from the settings file"
                 : g_gameWnd          ? "the game window's monitor"
                                      : "primary, no game window",
                 mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
            g_lastMonitor = mon;
            g_lastLoggedRect = mi.rcMonitor;
        }
    } else if (g_monitorTime < 0) {
        slog("GetMonitorInfo failed (error %lu), using %ldx%ld at 0,0", GetLastError(),
             g_monitorRect.right, g_monitorRect.bottom);
    }
}

// Returns the 3D monitor's rectangle. fresh = 1 always re-reads it.
static RECT current_monitor(int fresh)
{
    EnterCriticalSection(&g_monLock);
    double t = now_seconds();
    if (fresh || g_monitorTime < 0 || t - g_monitorTime >= MONITOR_REFRESH_S) {
        refresh_monitor(t);
        g_monitorTime = t;
    }
    RECT r = g_monitorRect;
    LeaveCriticalSection(&g_monLock);
    return r;
}

// ---------------------------------------------------------------- generic slot stubs

static SlotStats g_facStats[NSLOTS], g_devStats[NSLOTS], g_wvStats[NSLOTS];

#define GENERIC(prefix, stats, label, i)                                                           \
    static uintptr_t prefix##_slot##i(void *self, uintptr_t a, uintptr_t b, uintptr_t c,           \
                                      uintptr_t d, uintptr_t e)                                    \
    {                                                                                              \
        if (should_log(&stats[i], label "[" #i "] (unmapped)"))                                    \
            slog(label "[%d] (vtbl+0x%02x, unmapped) self=%p a=0x%llx b=0x%llx c=0x%llx "          \
                 "stack5=0x%llx stack6=0x%llx -> returning a",                                     \
                 i, i * 8, self, (unsigned long long)a, (unsigned long long)b,                     \
                 (unsigned long long)c, (unsigned long long)d, (unsigned long long)e);             \
        return a;                                                                                  \
    }

#define GENERIC_SET(prefix, stats, label)                                                          \
    GENERIC(prefix, stats, label, 0) GENERIC(prefix, stats, label, 1)                              \
    GENERIC(prefix, stats, label, 2) GENERIC(prefix, stats, label, 3)                              \
    GENERIC(prefix, stats, label, 4) GENERIC(prefix, stats, label, 5)                              \
    GENERIC(prefix, stats, label, 6) GENERIC(prefix, stats, label, 7)                              \
    GENERIC(prefix, stats, label, 8) GENERIC(prefix, stats, label, 9)                              \
    GENERIC(prefix, stats, label, 10) GENERIC(prefix, stats, label, 11)                            \
    GENERIC(prefix, stats, label, 12) GENERIC(prefix, stats, label, 13)                            \
    GENERIC(prefix, stats, label, 14) GENERIC(prefix, stats, label, 15)                            \
    GENERIC(prefix, stats, label, 16) GENERIC(prefix, stats, label, 17)                            \
    GENERIC(prefix, stats, label, 18) GENERIC(prefix, stats, label, 19)                            \
    GENERIC(prefix, stats, label, 20) GENERIC(prefix, stats, label, 21)                            \
    GENERIC(prefix, stats, label, 22) GENERIC(prefix, stats, label, 23)                            \
    GENERIC(prefix, stats, label, 24) GENERIC(prefix, stats, label, 25)                            \
    GENERIC(prefix, stats, label, 26) GENERIC(prefix, stats, label, 27)                            \
    GENERIC(prefix, stats, label, 28) GENERIC(prefix, stats, label, 29)                            \
    GENERIC(prefix, stats, label, 30) GENERIC(prefix, stats, label, 31)

GENERIC_SET(fac, g_facStats, "factory")
GENERIC_SET(dev, g_devStats, "device")
GENERIC_SET(wv, g_wvStats, "weaver")

#define SLOT_LIST(prefix)                                                                          \
    { (void *)prefix##_slot0, (void *)prefix##_slot1, (void *)prefix##_slot2,                       \
      (void *)prefix##_slot3, (void *)prefix##_slot4, (void *)prefix##_slot5,                       \
      (void *)prefix##_slot6, (void *)prefix##_slot7, (void *)prefix##_slot8,                       \
      (void *)prefix##_slot9, (void *)prefix##_slot10, (void *)prefix##_slot11,                     \
      (void *)prefix##_slot12, (void *)prefix##_slot13, (void *)prefix##_slot14,                    \
      (void *)prefix##_slot15, (void *)prefix##_slot16, (void *)prefix##_slot17,                    \
      (void *)prefix##_slot18, (void *)prefix##_slot19, (void *)prefix##_slot20,                    \
      (void *)prefix##_slot21, (void *)prefix##_slot22, (void *)prefix##_slot23,                    \
      (void *)prefix##_slot24, (void *)prefix##_slot25, (void *)prefix##_slot26,                    \
      (void *)prefix##_slot27, (void *)prefix##_slot28, (void *)prefix##_slot29,                    \
      (void *)prefix##_slot30, (void *)prefix##_slot31 }

static const char *slot_name(const char *obj, int i);

// ---------------------------------------------------------------- objects

typedef struct { void **vtbl; } Factory;
typedef struct { void **vtbl; } Device;
typedef struct {
    void **vtbl;
    ID3D12Resource *input;
    int inputFormat;
    ID3D12Resource *output;
    ID3D12GraphicsCommandList *cmdList;
    uint64_t lastInputW, lastOutputW;
    unsigned lastInputH, lastOutputH;
} Weaver;

// MSVC std::shared_ptr control block (_Ref_count_base): vtbl {_Destroy, _Delete_this, dtor, _Get_deleter},
// uses @+8, weaks @+0xC.
typedef struct { void **vtbl; volatile LONG uses; volatile LONG weaks; Weaver *obj; } RefCount;

static void *g_facVtbl[NSLOTS] = SLOT_LIST(fac);
static void *g_devVtbl[NSLOTS] = SLOT_LIST(dev);
static void *g_wvVtbl[NSLOTS] = SLOT_LIST(wv);

static Factory g_factory = { g_facVtbl };

// ---- device (known slots)

// vtbl+0x08: eye position in millimetres. eye 0 = left, 1 = right. Returns out.
// The game polls this ~110 times per second; the in-game Depth setting scales the result.
static float *dev_GetEyePosition(Device *self, float *out, int eye)
{
    // Nominal viewer: 64 mm IPD, centred, 600 mm in front of the screen.
    out[0] = eye == 0 ? -32.0f : 32.0f;
    out[1] = 0.0f;
    out[2] = 600.0f;
    if (should_log(&g_devStats[1], "device.GetEyePosition"))
        slog("device.GetEyePosition(eye=%d) -> %.1f %.1f %.1f mm", eye, out[0], out[1], out[2]);
    return out;
}

// vtbl+0x10: panel resolution in pixels (the game truncates to int). Returns out.
static float *dev_GetResolution(Device *self, float *out)
{
    RECT r = current_monitor(0);
    out[0] = (float)(r.right - r.left);
    out[1] = (float)(r.bottom - r.top);
    if (should_log(&g_devStats[2], "device.GetResolution"))
        slog("device.GetResolution -> %.0f x %.0f", out[0], out[1]);
    return out;
}

// vtbl+0x18: float2, the 3D panel's physical size in cm as the CyberpunkVR port reads it (the game keeps x * 0.01,
// i.e. metres). Answers a real 27" 16:9 Odyssey 3D, the hardware the game's 3D Mode was tuned on. The game reads
// it every stereo frame, but world depth showed no visible change between 10000 and 120 x 67.5, so it isn't a
// setting.
#define PANEL_WIDTH_CM 59.77f
#define PANEL_HEIGHT_CM 33.62f
static float *dev_GetFloat2_18(Device *self, float *out)
{
    out[0] = PANEL_WIDTH_CM;
    out[1] = PANEL_HEIGHT_CM;
    if (should_log(&g_devStats[3], "device.GetFloat2_18"))
        slog("device.GetFloat2_18 -> %.2f %.2f (27\" panel in cm; the game keeps x*0.01 = %.4f)", out[0], out[1],
             out[0] * 0.01);
    return out;
}

// vtbl+0x30: desktop rectangle of the 3D monitor, compared against the window's DXGI output.
// Layout from the game's comparison: int64 {top, left, right, bottom}. Returns out.
static int64_t *dev_GetMonitorRect(Device *self, int64_t *out)
{
    RECT r = current_monitor(1);
    out[0] = r.top;
    out[1] = r.left;
    out[2] = r.right;
    out[3] = r.bottom;
    if (should_log(&g_devStats[6], "device.GetMonitorRect"))
        slog("device.GetMonitorRect -> top=%lld left=%lld right=%lld bottom=%lld",
             (long long)out[0], (long long)out[1], (long long)out[2], (long long)out[3]);
    return out;
}

// vtbl+0x48: called 400-900 times per second from many threads with a bool (always 0 so far).
// The game ignores the return value. Likely a tracking or activity hint for the real panel.
static uintptr_t dev_Hint48(Device *self, uintptr_t flag)
{
    if (should_log(&g_devStats[9], "device.Hint48")) slog("device.Hint48(%llu)", (unsigned long long)flag);
    return 0;
}

// vtbl+0x50: is the 3D display available.
static unsigned char dev_IsAvailable(Device *self)
{
    if (should_log(&g_devStats[10], "device.IsAvailable")) slog("device.IsAvailable -> true");
    return 1;
}

// vtbl+0x60: scalar deleting destructor (flags & 1 = free memory).
static void *dev_Destroy(Device *self, unsigned flags)
{
    slog("device.Destroy(flags=%u) self=%p", flags, (void *)self);
    if (flags & 1) HeapFree(GetProcessHeap(), 0, self);
    return self;
}

// ---- weaver (known slots; unused while SBSDebug is on)

// vtbl+0x10: weave into the output at (x, y), size w x h. Draws nothing.
static void wv_Weave(Weaver *self, unsigned w, unsigned h, int x, int y)
{
    if (should_log(&g_wvStats[2], "weaver.Weave"))
        slog("weaver.Weave(w=%u h=%u x=%d y=%d) input=%p output=%p cmdList=%p (no-op)", w, h, x, y,
             (void *)self->input, (void *)self->output, (void *)self->cmdList);
}

// vtbl+0x20: the stereo input texture (both eyes) and its DXGI format.
static void wv_SetInputViewTexture(Weaver *self, ID3D12Resource *tex, int format)
{
    self->input = tex;
    self->inputFormat = format;
    int full = should_log(&g_wvStats[4], "weaver.SetInputViewTexture");
    if (tex && g_log) {
        D3D12_RESOURCE_DESC d;
        tex->lpVtbl->GetDesc(tex, &d);
        if (full || d.Width != self->lastInputW || d.Height != self->lastInputH) {
            slog("weaver.SetInputViewTexture(format=%d %s)", format, dxgi_format_name(format));
            describe_resource("input", tex);
            self->lastInputW = d.Width;
            self->lastInputH = d.Height;
        }
    } else if (full) {
        slog("weaver.SetInputViewTexture(NULL, format=%d)", format);
    }
}

// vtbl+0x30: the frame buffer to weave into.
static void wv_SetOutputFrameBuffer(Weaver *self, ID3D12Resource *tex)
{
    self->output = tex;
    int full = should_log(&g_wvStats[6], "weaver.SetOutputFrameBuffer");
    if (tex && g_log) {
        D3D12_RESOURCE_DESC d;
        tex->lpVtbl->GetDesc(tex, &d);
        if (full || d.Width != self->lastOutputW || d.Height != self->lastOutputH) {
            slog("weaver.SetOutputFrameBuffer");
            describe_resource("output", tex);
            self->lastOutputW = d.Width;
            self->lastOutputH = d.Height;
        }
    } else if (full) {
        slog("weaver.SetOutputFrameBuffer(NULL)");
    }
}

// vtbl+0x38: the command list to record into this frame.
static void wv_SetCommandList(Weaver *self, ID3D12GraphicsCommandList *cl)
{
    self->cmdList = cl;
    if (should_log(&g_wvStats[7], "weaver.SetCommandList")) slog("weaver.SetCommandList(%p)", (void *)cl);
}

// ---- shared_ptr control block for the weaver

static void rc_Destroy(RefCount *rc)
{
    slog("weaver shared_ptr: destroy object %p", (void *)rc->obj);
    if (rc->obj) HeapFree(GetProcessHeap(), 0, rc->obj);
    rc->obj = NULL;
}

static void rc_DeleteThis(RefCount *rc)
{
    slog("weaver shared_ptr: free control block %p", (void *)rc);
    HeapFree(GetProcessHeap(), 0, rc);
}

// Virtual destructor slot of _Ref_count_base (scalar deleting form). shared_ptr itself never
// calls it (it uses _Delete_this); it is here so the vtable layout is complete.
static RefCount *rc_Dtor(RefCount *rc, unsigned flags)
{
    slog("weaver shared_ptr: destructor called (flags=%u) %p", flags, (void *)rc);
    if (flags & 1) HeapFree(GetProcessHeap(), 0, rc);
    return rc;
}

static void *rc_GetDeleter(RefCount *rc, const void *typeinfo) { return NULL; }

// MSVC _Ref_count_base virtuals in order: _Destroy, _Delete_this, ~_Ref_count_base, _Get_deleter.
static void *g_rcVtbl[4] = { (void *)rc_Destroy, (void *)rc_DeleteThis, (void *)rc_Dtor, (void *)rc_GetDeleter };

// ---- factory (known slots)

// vtbl+0x00: create the device object. Writes it to *out and returns out.
static Device **fac_CreateDevice(Factory *self, Device **out)
{
    Device *dev = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Device));
    if (dev) dev->vtbl = g_devVtbl;
    *out = dev;
    slog("factory.CreateDevice -> device %p", (void *)dev);
    return out;
}

typedef struct { void *ptr; RefCount *rc; } SharedPtr;

// vtbl+0x10: create the D3D12 weaver. Returns a std::shared_ptr by hidden pointer.
// Args from the game: device, ID3D12Device, ID3D12CommandAllocator, queue-or-similar, 0, 0, HWND.
static SharedPtr *fac_CreateWeaver(Factory *self, SharedPtr *ret, Device *dev, IUnknown *d3dDevice,
                                   IUnknown *allocator, void *arg5, uintptr_t arg6, uintptr_t arg7,
                                   HWND hwnd)
{
    Weaver *wv = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Weaver));
    RefCount *rc = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RefCount));
    if (!wv || !rc) {
        // An empty shared_ptr: the game treats a null weaver as "no weaving".
        if (wv) HeapFree(GetProcessHeap(), 0, wv);
        if (rc) HeapFree(GetProcessHeap(), 0, rc);
        ret->ptr = NULL;
        ret->rc = NULL;
        slog("factory.CreateWeaver: out of memory, returning an empty weaver");
        return ret;
    }
    wv->vtbl = g_wvVtbl;
    rc->vtbl = g_rcVtbl;
    rc->uses = 1;
    rc->weaks = 1;
    rc->obj = wv;
    ret->ptr = wv;
    ret->rc = rc;

    // No GetWindowText here: it sends WM_GETTEXT and can block on a busy window thread.
    slog("factory.CreateWeaver dev=%p d3dDevice=%p allocator=%p arg5=%p arg6=0x%llx arg7=0x%llx hwnd=%p -> weaver %p",
         (void *)dev, (void *)d3dDevice, (void *)allocator, arg5, (unsigned long long)arg6,
         (unsigned long long)arg7, (void *)hwnd, (void *)wv);
    if (hwnd && IsWindow(hwnd)) log_window("  weaver window", hwnd);
    return ret;
}

static void install_known_slots(void)
{
    g_facVtbl[0] = (void *)fac_CreateDevice;
    g_facVtbl[2] = (void *)fac_CreateWeaver;

    g_devVtbl[1] = (void *)dev_GetEyePosition;
    g_devVtbl[2] = (void *)dev_GetResolution;
    g_devVtbl[3] = (void *)dev_GetFloat2_18;
    g_devVtbl[6] = (void *)dev_GetMonitorRect;
    g_devVtbl[9] = (void *)dev_Hint48;
    g_devVtbl[10] = (void *)dev_IsAvailable;
    g_devVtbl[12] = (void *)dev_Destroy;

    g_wvVtbl[2] = (void *)wv_Weave;
    g_wvVtbl[4] = (void *)wv_SetInputViewTexture;
    g_wvVtbl[6] = (void *)wv_SetOutputFrameBuffer;
    g_wvVtbl[7] = (void *)wv_SetCommandList;
}

static const char *slot_name(const char *obj, int i)
{
    static const char *dev[NSLOTS] = { [1] = "GetEyePosition", [2] = "GetResolution", [3] = "GetFloat2_18",
                                       [6] = "GetMonitorRect", [9] = "Hint48", [10] = "IsAvailable",
                                       [12] = "Destroy" };
    static const char *fac[NSLOTS] = { [0] = "CreateDevice", [2] = "CreateWeaver" };
    static const char *wv[NSLOTS] = { [2] = "Weave", [4] = "SetInputViewTexture", [6] = "SetOutputFrameBuffer",
                                      [7] = "SetCommandList" };
    const char *n = obj[0] == 'd' ? dev[i] : obj[0] == 'f' ? fac[i] : wv[i];
    return n ? n : "(unmapped)";
}

static void log_call_totals(const char *obj, SlotStats *stats)
{
    for (int i = 0; i < NSLOTS; i++)
        if (stats[i].calls)
            slog("  %s[%d] %s: %lld calls", obj, i, slot_name(obj, i), (long long)stats[i].calls);
}

// ---------------------------------------------------------------- exports

// The registry key that points games at the SDK is shared by every game with Odyssey 3D support,
// and this stand-in only implements what Cyberpunk 2077 uses. Other programs get no factory.
// BRAINDANCE3D_ANY_EXE=1 lifts this for the test suite.
static int g_hostAllowed;

static int is_allowed_host(void)
{
    wchar_t exe[MAX_PATH], env[8];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    const wchar_t *name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    if (_wcsicmp(name, L"Cyberpunk2077.exe") == 0) return 1;
    n = GetEnvironmentVariableW(L"BRAINDANCE3D_ANY_EXE", env, 8);
    return n > 0 && n < 8 && wcscmp(env, L"1") == 0;
}

EXPORT Factory *create_odyssey3d_factory_instance(void)
{
    if (!g_hostAllowed) {
        slog("create_odyssey3d_factory_instance -> NULL (this process is not Cyberpunk2077.exe)");
        return NULL;
    }
    slog("create_odyssey3d_factory_instance -> %p", (void *)&g_factory);
    return &g_factory;
}

EXPORT void destroy_odyssey3d_factory_instance(Factory *f)
{
    slog("destroy_odyssey3d_factory_instance(%p)", (void *)f);
}

// Lets tools and bug reports identify this build.
EXPORT const char *braindance3d_version(void) { return BD3D_VERSION_STR; }

// How many times the game has called one device slot (vtable index), or -1 for a slot that doesn't exist. The
// RED4ext plugin's menu squeeze reads two of them once per game frame: GetEyePosition (1) is asked for both eyes
// on every frame rendered in stereo and not at all on a flat one (menus, the map, loading screens); IsAvailable
// (10) is asked once per frame in 2D gameplay and twice in stereo. The others are for the tests and the log.
EXPORT LONG64 braindance3d_device_calls(int slot)
{
    return slot >= 0 && slot < NSLOTS ? g_devStats[slot].calls : -1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logLock);
        InitializeCriticalSection(&g_monLock);
        find_install_dir();
        read_settings();
        // Decide the host first: another program that loads the stand-in must not rotate away the game's log.
        g_hostAllowed = is_allowed_host();
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_t0);
        if (g_hostAllowed) log_open();
        wchar_t exe[MAX_PATH] = L"";
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        char b1[MAX_PATH * 3], b2[MAX_PATH * 3];
        slog("==== 3D Braindance Edition, Odyssey3DSDK stand-in v%s loaded into %s (pid %lu)",
             BD3D_VERSION_STR, utf8(exe, b1, sizeof(b1)), GetCurrentProcessId());
        slog("install folder %s, log level %d (" "Braindance3D-Settings.ini [Log] Level: 0 off, 1 normal, 2 verbose)",
             utf8(g_installDir, b2, sizeof(b2)), g_logLevel);
        slog("log file %s", utf8(g_logPath, b1, sizeof(b1)));
        slog("settings: monitor %s; host %s", g_monitorPrimaryOnly ? "primary" : "auto (game window's monitor)",
             g_hostAllowed ? "allowed" : "NOT Cyberpunk2077.exe, the SDK will not be offered");
        install_known_slots();
    } else if (reason == DLL_PROCESS_DETACH) {
        // At process exit other threads were killed, possibly while holding the log lock, so only
        // log if the lock is free. Every line is already flushed, so skipping loses nothing else.
        if (TryEnterCriticalSection(&g_logLock)) {
            slog("==== unloading%s. Call totals:", reserved ? " (process exit)" : "");
            log_call_totals("factory", g_facStats);
            log_call_totals("device", g_devStats);
            log_call_totals("weaver", g_wvStats);
            // At process exit the lines are already flushed and Windows closes the file.
            if (g_log && !reserved) fclose(g_log);
            if (!reserved) g_log = NULL;
            LeaveCriticalSection(&g_logLock);
            if (!reserved) {
                // FreeLibrary with the lock free: nothing can call in any more.
                DeleteCriticalSection(&g_monLock);
                DeleteCriticalSection(&g_logLock);
            }
        }
    }
    return TRUE;
}
