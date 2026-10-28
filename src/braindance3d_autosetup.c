// 3D Braindance Edition: RED4ext plugin that registers the Odyssey 3D SDK stand-in automatically.
//
// The game finds the SDK through HKCU\SOFTWARE\Samsung\Odyssey3DHub\InstallPath. Mod managers can
// only copy files, so without this plugin the user has to run Install-Braindance3D.bat once.
// RED4ext loads this plugin when the game starts, before the game looks for the SDK, and the plugin
// points InstallPath at <game>\bin\x64\Braindance3D with the same rules as the installer script:
//   - nothing changes when the SDK stand-in isn't there,
//   - another copy of this mod, or an InstallPath with no SDK in it, is replaced (and remembered),
//   - Samsung's Odyssey 3D Hub (or any other working SDK) is never replaced.
// It doesn't hook or touch the game in any other way.
//
// License: MIT (see LICENSE).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "version.h"

#define EXPORT __declspec(dllexport)

// ---------------------------------------------------------------- RED4ext plugin ABI (API v1, reported as v0)
// Layouts from RED4ext.SDK include/RED4ext/Api/v1 (SemVer.hpp, FileVer.hpp, PluginInfo.hpp, EMainReason.hpp).

typedef struct { uint32_t type; uint32_t number; } R4ePrerelease;
typedef struct { uint8_t major; uint16_t minor; uint32_t patch; R4ePrerelease prerelease; } R4eSemVer;
typedef struct { uint16_t major, minor, build, revision; } R4eFileVer;
typedef struct {
    R4eSemVer sdk;
    const wchar_t *name;
    const wchar_t *author;
    R4eSemVer version;
    R4eFileVer runtime;
} R4ePluginInfo;

enum { R4E_MAIN_LOAD = 0, R4E_MAIN_UNLOAD = 1 };
#define R4E_API_VERSION_1_COMPAT_0 0 // API v0 and v1 are compatible; v0 also loads in older RED4ext

// ---------------------------------------------------------------- settings shared with the installer

#define DEFAULT_KEY L"SOFTWARE\\Samsung\\Odyssey3DHub"
#define VALUE_INSTALL_PATH L"InstallPath"
#define VALUE_MARKER L"Braindance3D_InstalledBy"
#define VALUE_OWN_PATH L"Braindance3D_InstallPath"
#define VALUE_PREVIOUS L"Braindance3D_PreviousInstallPath"
#define NO_PREVIOUS L"<none>"
#define SDK_RELATIVE L"\\GameLauncherData\\core\\Odyssey3DSDK.dll"
#define SETTINGS_FILE L"Braindance3D-Settings.ini"
#define PRODUCT_NAME L"3D Braindance Edition"

// ---------------------------------------------------------------- logging

static FILE *g_log;

static const char *utf8(const wchar_t *w, char *buf, int size)
{
    if (!w || !WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, size, NULL, NULL)) buf[0] = 0;
    return buf;
}

static void alog(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "%04d-%02d-%02d %02d:%02d:%02d.%03d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// Opens <dir>\Braindance3D-autosetup.log for appending, or %LOCALAPPDATA%\Braindance3D\ if dir is read-only.
static void open_log(const wchar_t *dir)
{
    wchar_t path[MAX_PATH];
    int n = _snwprintf(path, MAX_PATH, L"%ls\\Braindance3D-autosetup.log", dir);
    if (n > 0 && n < MAX_PATH) g_log = _wfopen(path, L"a");
    if (g_log) {
        // A few lines per game start; start over once it passes 256 KB.
        fseek(g_log, 0, SEEK_END);
        if (ftell(g_log) > 256 * 1024) g_log = _wfreopen(path, L"w", g_log);
        return;
    }
    wchar_t local[MAX_PATH];
    DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (m == 0 || m >= MAX_PATH - 48) return;
    wcscat(local, L"\\Braindance3D");
    CreateDirectoryW(local, NULL);
    wcscat(local, L"\\Braindance3D-autosetup.log");
    g_log = _wfopen(local, L"a");
}

// ---------------------------------------------------------------- helpers

static int file_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Whether a path is definitely gone: "not found" on a local fixed drive that is there. An offline USB
// drive, a network share, a quoted path or an access error is "unknown", never "gone", so the plugin
// can't take over someone else's SDK because its drive happens to be unavailable.
static int definitely_missing(const wchar_t *path)
{
    if (!path || !path[0] || wcschr(path, L'"')) return 0;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return 0;
    DWORD err = GetLastError();
    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) return 0;
    wchar_t full[MAX_PATH];
    DWORD n = GetFullPathNameW(path, MAX_PATH, full, NULL);
    if (n == 0 || n >= MAX_PATH) return 0;
    if (!iswalpha(full[0]) || full[1] != L':' || full[2] != L'\\') return 0; // UNC and device paths
    wchar_t root[4] = { full[0], L':', L'\\', 0 };
    return GetDriveTypeW(root) == DRIVE_FIXED && GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES;
}

// Full path without a trailing backslash (except for a drive root). Returns 0 on failure.
static int normalize(const wchar_t *in, wchar_t *out)
{
    DWORD n = GetFullPathNameW(in, MAX_PATH, out, NULL);
    if (n == 0 || n >= MAX_PATH) return 0;
    size_t len = wcslen(out);
    while (len > 3 && (out[len - 1] == L'\\' || out[len - 1] == L'/')) out[--len] = 0;
    return 1;
}

static int same_path(const wchar_t *a, const wchar_t *b)
{
    wchar_t na[MAX_PATH], nb[MAX_PATH];
    if (!a || !b || !a[0] || !b[0]) return 0;
    if (!normalize(a, na) || !normalize(b, nb)) return 0;
    return _wcsicmp(na, nb) == 0;
}

static int sdk_path(const wchar_t *dir, wchar_t *out)
{
    int n = _snwprintf(out, MAX_PATH, L"%ls" SDK_RELATIVE, dir);
    return n > 0 && n < MAX_PATH;
}

// The SDK DLL's ProductName from its version resource, or "" if it has none.
static void product_name(const wchar_t *dll, wchar_t *out, int size)
{
    out[0] = 0;
    DWORD handle = 0, len = GetFileVersionInfoSizeW(dll, &handle);
    if (!len) return;
    void *data = HeapAlloc(GetProcessHeap(), 0, len);
    if (!data) return;
    if (GetFileVersionInfoW(dll, 0, len, data)) {
        struct { WORD lang, cp; } *tr;
        UINT trLen = 0;
        if (VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void **)&tr, &trLen) && trLen >= sizeof(*tr)) {
            wchar_t q[64];
            _snwprintf(q, 64, L"\\StringFileInfo\\%04x%04x\\ProductName", tr->lang, tr->cp);
            q[63] = 0;
            wchar_t *v = NULL;
            UINT vLen = 0;
            if (VerQueryValueW(data, q, (void **)&v, &vLen) && v && vLen) {
                wcsncpy(out, v, size - 1);
                out[size - 1] = 0;
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, data);
}

typedef struct { int exists; int ours; int gone; wchar_t product[128]; } SdkInfo;

// Whether a folder holds an SDK DLL, and whether that DLL is this mod's.
static SdkInfo sdk_info(const wchar_t *dir)
{
    SdkInfo info = {0};
    wchar_t dll[MAX_PATH], settings[MAX_PATH];
    if (!dir || !dir[0] || !sdk_path(dir, dll)) return info;
    if (!file_exists(dll)) { info.gone = definitely_missing(dll); return info; }
    info.exists = 1;
    product_name(dll, info.product, 128);
    if (wcscmp(info.product, PRODUCT_NAME) == 0) info.ours = 1;
    int n = _snwprintf(settings, MAX_PATH, L"%ls\\" SETTINGS_FILE, dir);
    if (n > 0 && n < MAX_PATH && file_exists(settings)) info.ours = 1;
    return info;
}

// Reads a string value into out (empty if missing), expanding REG_EXPAND_SZ.
// Returns 1 if read, 0 if missing, -1 if present but unusable (not a string, or MAX_PATH or longer),
// so the caller never overwrites a value it couldn't read.
static int reg_get(HKEY key, const wchar_t *name, wchar_t *out)
{
    out[0] = 0;
    if (!key) return 0;
    wchar_t raw[MAX_PATH];
    DWORD type = 0, size = sizeof(raw);
    LONG rc = RegQueryValueExW(key, name, NULL, &type, (BYTE *)raw, &size);
    if (rc == ERROR_FILE_NOT_FOUND) return 0;
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return -1;
    DWORD len = size / sizeof(wchar_t);
    if (len > 0 && raw[len - 1] == 0) len--;
    if (len >= MAX_PATH) return -1;
    raw[len] = 0;
    if (type == REG_EXPAND_SZ) {
        DWORD n = ExpandEnvironmentStringsW(raw, out, MAX_PATH);
        if (n == 0 || n > MAX_PATH) { out[0] = 0; return -1; }
    } else {
        memcpy(out, raw, (len + 1) * sizeof(wchar_t));
    }
    return 1;
}

static LONG reg_set(HKEY key, const wchar_t *name, const wchar_t *value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

// ---------------------------------------------------------------- the decision

typedef enum {
    AUTO_ALREADY = 0,     // InstallPath already points here
    AUTO_INSTALLED = 1,   // InstallPath now points here
    AUTO_NO_SDK = 2,      // the stand-in isn't in the target folder: nothing changed
    AUTO_FOREIGN_SDK = 3, // another working SDK (Samsung's) is registered: nothing changed
    AUTO_SAMSUNG_DEFAULT = 4, // Samsung's Hub is in its default folder: nothing changed
    AUTO_ERROR = 5,       // registry error: nothing (or not everything) changed
} AutoResult;

// Points HKCU\<subkey>\InstallPath at target if that is safe. samsungDir is Samsung's default folder.
// Kept free of game state so the tests can run it against a scratch registry key.
static AutoResult auto_register(const wchar_t *subkey, const wchar_t *target, const wchar_t *samsungDir)
{
    char b1[MAX_PATH * 3], b2[MAX_PATH * 3];
    if (!sdk_info(target).exists) {
        alog("The SDK stand-in is missing under %s. Nothing changed.", utf8(target, b1, sizeof(b1)));
        return AUTO_NO_SDK;
    }

    HKEY key = NULL;
    wchar_t current[MAX_PATH] = L"", own[MAX_PATH] = L"", previous[MAX_PATH] = L"", marker[MAX_PATH] = L"";
    LONG openRc = RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &key);
    if (openRc != ERROR_SUCCESS && openRc != ERROR_FILE_NOT_FOUND) {
        alog("Couldn't read HKCU\\%s (error %ld). Nothing changed.", utf8(subkey, b1, sizeof(b1)), openRc);
        return AUTO_ERROR;
    }
    if (openRc == ERROR_SUCCESS) {
        int rcCurrent = reg_get(key, VALUE_INSTALL_PATH, current);
        reg_get(key, VALUE_OWN_PATH, own);
        int rcPrevious = reg_get(key, VALUE_PREVIOUS, previous);
        reg_get(key, VALUE_MARKER, marker);
        RegCloseKey(key);
        if (rcCurrent < 0) {
            alog("InstallPath is set but isn't a readable path (not a string, or too long). It may belong to "
                 "Samsung's Odyssey 3D Hub, so nothing changed. To use this mod anyway, run "
                 "Install-Braindance3D.bat -Force.");
            return AUTO_FOREIGN_SDK;
        }
        if (rcPrevious < 0) {
            alog("The saved previous InstallPath can't be read, so it couldn't be kept. Nothing changed.");
            return AUTO_ERROR;
        }
    }
    {
        char b3[MAX_PATH * 3], b4[MAX_PATH * 3];
        alog("Registry before: InstallPath='%s' ownPath='%s' previous='%s' installedBy='%s'", utf8(current, b1, sizeof(b1)),
             utf8(own, b2, sizeof(b2)), utf8(previous, b3, sizeof(b3)), utf8(marker, b4, sizeof(b4)));
    }

    if (current[0] && same_path(current, target)) {
        alog("InstallPath already points at %s. Nothing to do.", utf8(target, b1, sizeof(b1)));
        return AUTO_ALREADY;
    }

    const wchar_t *keep = NO_PREVIOUS;
    if (current[0]) {
        SdkInfo other = sdk_info(current);
        if ((own[0] && same_path(current, own)) || other.ours) {
            alog("Replacing another copy of this mod at %s.", utf8(current, b1, sizeof(b1)));
            if (previous[0]) keep = previous;
        } else if (!other.exists && other.gone) {
            SdkInfo samsung = samsungDir && samsungDir[0] ? sdk_info(samsungDir) : (SdkInfo){0};
            if (samsung.exists && !samsung.ours) {
                alog("The current InstallPath %s is gone, but Samsung's Odyssey 3D Hub is installed in %s. Not "
                     "changing anything. To use this mod anyway, run Install-Braindance3D.bat -Force.",
                     utf8(current, b1, sizeof(b1)), utf8(samsungDir, b2, sizeof(b2)));
                return AUTO_SAMSUNG_DEFAULT;
            }
            alog("The current InstallPath %s has no SDK in it, so nothing uses it. Saving it for the uninstaller.",
                 utf8(current, b1, sizeof(b1)));
            keep = current;
        } else if (!other.exists) {
            alog("The current InstallPath %s can't be checked right now (offline or network drive, or no access). "
                 "It may be Samsung's Odyssey 3D Hub, so nothing changed.", utf8(current, b1, sizeof(b1)));
            return AUTO_FOREIGN_SDK;
        } else {
            alog("Another Odyssey 3D SDK is registered at %s (product '%s'), probably Samsung's Odyssey 3D Hub. "
                 "Not changing it. To use this mod anyway, run Install-Braindance3D.bat -Force.",
                 utf8(current, b1, sizeof(b1)), utf8(other.product, b2, sizeof(b2)));
            return AUTO_FOREIGN_SDK;
        }
    } else if (samsungDir && samsungDir[0]) {
        SdkInfo samsung = sdk_info(samsungDir);
        if (samsung.exists && !samsung.ours) {
            alog("Samsung's Odyssey 3D Hub is installed in %s. Not changing anything. To use this mod anyway, run "
                 "Install-Braindance3D.bat -Force.", utf8(samsungDir, b1, sizeof(b1)));
            return AUTO_SAMSUNG_DEFAULT;
        }
    }

    wchar_t normTarget[MAX_PATH];
    if (!normalize(target, normTarget)) return AUTO_ERROR;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) {
        alog("Couldn't open or create HKCU\\%s (error %ld). Nothing changed.", utf8(subkey, b1, sizeof(b1)), rc);
        return AUTO_ERROR;
    }
    // Bookkeeping first, so an interrupted run can still be undone by the uninstaller.
    wchar_t keepCopy[MAX_PATH];
    wcsncpy(keepCopy, keep, MAX_PATH - 1);
    keepCopy[MAX_PATH - 1] = 0;
    rc = reg_set(key, VALUE_PREVIOUS, keepCopy);
    if (rc == ERROR_SUCCESS) rc = reg_set(key, VALUE_OWN_PATH, normTarget);
    if (rc == ERROR_SUCCESS) rc = reg_set(key, VALUE_MARKER, BD3D_VERSION_WSTR L" (RED4ext auto-setup)");
    if (rc == ERROR_SUCCESS) rc = reg_set(key, VALUE_INSTALL_PATH, normTarget);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        alog("Writing the registry failed (error %ld).", rc);
        return AUTO_ERROR;
    }
    alog("Registered: InstallPath='%s' (previous saved: '%s').", utf8(normTarget, b1, sizeof(b1)),
         utf8(keepCopy, b2, sizeof(b2)));
    return AUTO_INSTALLED;
}

// <exe folder>\Braindance3D, where the mod puts the stand-in (<game>\bin\x64\Braindance3D).
static int default_target(wchar_t *out)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (!slash) return 0;
    *slash = 0;
    int m = _snwprintf(out, MAX_PATH, L"%ls\\Braindance3D", exe);
    return m > 0 && m < MAX_PATH;
}

static void default_samsung_dir(wchar_t *out)
{
    out[0] = 0;
    wchar_t pf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 20) return;
    _snwprintf(out, MAX_PATH, L"%ls\\Odyssey 3D Hub", pf);
    out[MAX_PATH - 1] = 0;
}

// ---------------------------------------------------------------- RED4ext exports

EXPORT void Query(R4ePluginInfo *info)
{
    info->sdk = (R4eSemVer){ 0, 5, 0, { 0, 0 } }; // RED4EXT_V1_SDK_VERSION_1_0_0_COMPAT_0_5_0
    info->name = L"3D Braindance Edition auto-setup";
    info->author = L"vl3c";
    info->version = (R4eSemVer){ BD3D_VERSION_MAJOR, BD3D_VERSION_MINOR, BD3D_VERSION_PATCH, { 0, 0 } };
    info->runtime = (R4eFileVer){ 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF }; // RED4EXT_V1_RUNTIME_VERSION_INDEPENDENT
}

EXPORT uint32_t Supports(void) { return R4E_API_VERSION_1_COMPAT_0; }

EXPORT int Main(HMODULE handle, uint8_t reason, const void *sdk)
{
    if (reason != R4E_MAIN_LOAD) return 1;
    wchar_t target[MAX_PATH], samsung[MAX_PATH];
    if (!default_target(target)) return 1;
    open_log(target);
    char b[MAX_PATH * 3];
    alog("==== 3D Braindance Edition auto-setup %s (RED4ext plugin), target %s", BD3D_VERSION_STR, utf8(target, b, sizeof(b)));
    default_samsung_dir(samsung);
    AutoResult r = auto_register(DEFAULT_KEY, target, samsung);
    alog("Result: %d", (int)r);
    if (g_log) { fclose(g_log); g_log = NULL; }
    return 1; // never stop the game from loading
}
