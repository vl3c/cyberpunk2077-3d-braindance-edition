// Black-box tests for build/Odyssey3DSDK.dll (the Odyssey 3D SDK stand-in).
//
// Every test copies the built DLL into a fresh "<temp>\<case>\GameLauncherData\core\" folder (the
// layout the game expects under the registry InstallPath), loads it with LoadLibraryEx like the game,
// calls the exports and the vtable slots with the game's calling convention (Microsoft x64, C++-style
// "this" first), and unloads it again, so each test sees a freshly initialised DLL.
//
// Run: test_dll.exe [--dll path\to\Odyssey3DSDK.dll] [--known-bugs] [--list] [name filters...]

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define TT_IMPLEMENTATION
#include <windows.h>
#include <d3d12.h>
#include <sddl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "testutil.h"
#include "../../src/version.h"

// The ProductName in src/version.rc; run-native-tests.sh passes it in with -DBD3D_PRODUCT_NAME.
#ifndef BD3D_PRODUCT_NAME
#define BD3D_PRODUCT_NAME "3D Braindance Edition"
#endif

// ===================================================================================== the ABI

typedef struct Obj { void **vtbl; } Obj;

// MSVC std::shared_ptr<T> as returned by value: {T* _Ptr; _Ref_count_base* _Rep}.
typedef struct RefCount { void **vtbl; volatile LONG uses; volatile LONG weaks; } RefCount;
typedef struct { Obj *ptr; RefCount *rc; } SharedPtr;

typedef Obj *(*PFN_create)(void);
typedef void (*PFN_destroy)(Obj *);
typedef const char *(*PFN_version)(void);

typedef Obj **(*PFN_CreateDevice)(Obj *self, Obj **out);
typedef SharedPtr *(*PFN_CreateWeaver)(Obj *self, SharedPtr *ret, Obj *dev, void *d3dDevice, void *allocator,
                                       void *arg5, uintptr_t arg6, uintptr_t arg7, HWND hwnd);
typedef float *(*PFN_GetFloat3I)(Obj *self, float *out, int eye);
typedef float *(*PFN_GetFloat2)(Obj *self, float *out);
typedef int64_t *(*PFN_GetRect)(Obj *self, int64_t *out);
typedef uintptr_t (*PFN_Hint)(Obj *self, uintptr_t flag);
typedef unsigned char (*PFN_IsAvailable)(Obj *self);
typedef void *(*PFN_ScalarDtor)(Obj *self, unsigned flags);
typedef void (*PFN_Weave)(Obj *self, unsigned w, unsigned h, int x, int y);
typedef void (*PFN_SetInput)(Obj *self, void *tex, int format);
typedef void (*PFN_SetPtr)(Obj *self, void *p);
typedef uintptr_t (*PFN_Generic)(Obj *self, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
typedef void (*PFN_RcVoid)(RefCount *rc);
typedef void *(*PFN_RcGetDeleter)(RefCount *rc, const void *typeinfo);

#define NSLOTS 32
#define SETTINGS_FILE L"Braindance3D-Settings.ini" // in the install folder
#define SLOT(obj, i, T) ((T)((obj)->vtbl[i]))

// Slots from docs/TECHNICAL.md.
enum { FAC_CREATE_DEVICE = 0, FAC_CREATE_WEAVER = 2 };
enum {
    DEV_GET_EYE_POSITION = 1, DEV_GET_RESOLUTION = 2, DEV_GET_FLOAT2_18 = 3, DEV_GET_MONITOR_RECT = 6,
    DEV_HINT48 = 9, DEV_IS_AVAILABLE = 10, DEV_DESTROY = 12
};
enum { WV_WEAVE = 2, WV_SET_INPUT = 4, WV_SET_OUTPUT = 6, WV_SET_CMDLIST = 7 };

static const int kFacKnown[] = { FAC_CREATE_DEVICE, FAC_CREATE_WEAVER };
static const int kDevKnown[] = { DEV_GET_EYE_POSITION, DEV_GET_RESOLUTION, DEV_GET_FLOAT2_18, DEV_GET_MONITOR_RECT,
                                 DEV_HINT48, DEV_IS_AVAILABLE, DEV_DESTROY };
static const int kWvKnown[] = { WV_WEAVE, WV_SET_INPUT, WV_SET_OUTPUT, WV_SET_CMDLIST };

static int is_known(const int *known, int n, int slot)
{
    for (int i = 0; i < n; i++)
        if (known[i] == slot) return 1;
    return 0;
}

// ===================================================================================== fixture

typedef struct {
    wchar_t root[TU_PATH]; // the install folder (what the registry InstallPath points at)
    wchar_t core[TU_PATH]; // root\GameLauncherData\core
    wchar_t dll[TU_PATH];
    wchar_t log[TU_PATH], oldlog[TU_PATH];
    HMODULE h;
    PFN_create create;
    PFN_destroy destroy;
    PFN_version version;
} Sdk;

static void sdk_unload(Sdk *s)
{
    if (!s->h) return;
    for (int i = 0; i < 8 && GetModuleHandleW(s->dll); i++) FreeLibrary(s->h);
    s->h = NULL;
}

static void sdk_unload_deferred(void *p) { sdk_unload((Sdk *)p); }

static void sdk_free_deferred(void *p) { free(p); }

// Lays out <parent>\<name>\GameLauncherData\core\Odyssey3DSDK.dll, plus the settings file when ini != NULL.
static Sdk *sdk_prepare_in(const wchar_t *parent, const wchar_t *name, const char *ini)
{
    tu_require_dll();
    Sdk *s = calloc(1, sizeof(Sdk));
    tt_defer(sdk_free_deferred, s);
    tu_join(s->root, TU_PATH, parent, name);
    tu_join(s->core, TU_PATH, s->root, L"GameLauncherData\\core");
    tu_join(s->dll, TU_PATH, s->core, L"Odyssey3DSDK.dll");
    tu_join(s->log, TU_PATH, s->root, L"braindance3d.log");
    tu_join(s->oldlog, TU_PATH, s->root, L"braindance3d.old.log");
    REQUIRE_MSG(tu_mkdirs(s->core), "mkdir %ls", s->core);
    REQUIRE_MSG(CopyFileW(tu_dll_path(), s->dll, FALSE), "copy %ls -> %ls failed (%lu)", tu_dll_path(), s->dll,
                GetLastError());
    if (ini) {
        wchar_t p[TU_PATH];
        tu_join(p, TU_PATH, s->root, SETTINGS_FILE);
        REQUIRE(tu_write_text(p, ini));
    }
    return s;
}

static Sdk *sdk_prepare(const char *ini) { return sdk_prepare_in(tu_tempdir(L"sdk"), L"Game Folder", ini); }

static void sdk_load(Sdk *s)
{
    REQUIRE_MSG(GetModuleHandleW(s->dll) == NULL, "DLL already loaded");
    s->h = LoadLibraryExW(s->dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    REQUIRE_MSG(s->h != NULL, "LoadLibraryEx(%ls) failed (%lu)", s->dll, GetLastError());
    tt_defer(sdk_unload_deferred, s);
    s->create = (PFN_create)(void *)GetProcAddress(s->h, "create_odyssey3d_factory_instance");
    s->destroy = (PFN_destroy)(void *)GetProcAddress(s->h, "destroy_odyssey3d_factory_instance");
    s->version = (PFN_version)(void *)GetProcAddress(s->h, "braindance3d_version");
    REQUIRE(s->create != NULL);
    REQUIRE(s->destroy != NULL);
}

static Sdk *sdk_loaded(const char *ini)
{
    Sdk *s = sdk_prepare(ini);
    sdk_load(s);
    return s;
}

static Obj *sdk_factory(Sdk *s)
{
    Obj *f = s->create();
    REQUIRE(f != NULL);
    REQUIRE(f->vtbl != NULL);
    return f;
}

static Obj *sdk_device(Sdk *s)
{
    Obj *f = sdk_factory(s);
    Obj *dev = NULL;
    Obj **r = SLOT(f, FAC_CREATE_DEVICE, PFN_CreateDevice)(f, &dev);
    REQUIRE(r == &dev);
    REQUIRE(dev != NULL);
    REQUIRE(dev->vtbl != NULL);
    return dev;
}

static void device_release(Obj *dev) { SLOT(dev, DEV_DESTROY, PFN_ScalarDtor)(dev, 1); }

// MSVC shared_ptr release: --uses == 0 -> _Destroy(); --weaks == 0 -> _Delete_this().
static void sp_release(SharedPtr *sp)
{
    if (!sp->rc) return;
    RefCount *rc = sp->rc;
    PFN_RcVoid destroy = (PFN_RcVoid)rc->vtbl[0], delete_this = (PFN_RcVoid)rc->vtbl[1];
    if (InterlockedDecrement(&rc->uses) == 0) {
        destroy(rc);
        if (InterlockedDecrement(&rc->weaks) == 0) delete_this(rc);
    }
    sp->ptr = NULL;
    sp->rc = NULL;
}

static RECT monitor_rect_of(HWND hwnd)
{
    HMONITOR m;
    if (hwnd) {
        m = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    } else {
        POINT origin = {0, 0};
        m = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    RECT r = {0, 0, 0, 0};
    if (GetMonitorInfoW(m, &mi)) r = mi.rcMonitor;
    return r;
}

static int rect_matches(const int64_t got[4], RECT want)
{
    return got[0] == want.top && got[1] == want.left && got[2] == want.right && got[3] == want.bottom;
}

// Polls GetMonitorRect until it reports `want` (the DLL caches the monitor for a short while).
static int wait_for_monitor(Obj *dev, RECT want, int64_t got[4], int timeout_ms)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        SLOT(dev, DEV_GET_MONITOR_RECT, PFN_GetRect)(dev, got);
        if (rect_matches(got, want)) return 1;
        if ((int)(GetTickCount() - t0) > timeout_ms) return 0;
        tu_pump();
        Sleep(50);
    }
}

// ===================================================================================== fake D3D12 objects

// Fake COM objects with counted AddRef/Release. Any other method lands in a trap that counts it.
typedef struct FakeCom {
    void **vtbl;
    LONG refs;
    LONG getdesc_calls, getdesc_bad_this, trap_calls;
    D3D12_RESOURCE_DESC desc;
} FakeCom;

#define FAKE_SLOTS 128
static void *g_fakeVtbl[FAKE_SLOTS];
static FakeCom *g_fakes[16];
static int g_nfakes;
static volatile LONG g_trapCalls;

static int is_fake(const void *p)
{
    for (int i = 0; i < g_nfakes; i++)
        if (g_fakes[i] == p) return 1;
    return 0;
}

static HRESULT STDMETHODCALLTYPE fake_qi(FakeCom *This, REFIID iid, void **out)
{
    if (out) *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fake_addref(FakeCom *This) { return is_fake(This) ? (ULONG)InterlockedIncrement(&This->refs) : 0; }
static ULONG STDMETHODCALLTYPE fake_release(FakeCom *This) { return is_fake(This) ? (ULONG)InterlockedDecrement(&This->refs) : 0; }
static uintptr_t fake_trap(void *This)
{
    InterlockedIncrement(&g_trapCalls);
    if (is_fake(This)) InterlockedIncrement(&((FakeCom *)This)->trap_calls);
    return 0;
}
// ID3D12Resource::GetDesc as the C++ ABI calls it: this in RCX, hidden return pointer in RDX.
static D3D12_RESOURCE_DESC *STDMETHODCALLTYPE fake_getdesc(FakeCom *This, D3D12_RESOURCE_DESC *ret)
{
    if (!is_fake(This)) {
        // Called with the wrong ABI (return pointer first): count it on every fake, fill nothing.
        for (int i = 0; i < g_nfakes; i++) InterlockedIncrement(&g_fakes[i]->getdesc_bad_this);
        return ret;
    }
    InterlockedIncrement(&This->getdesc_calls);
    if (ret) *ret = This->desc;
    return ret;
}

static void fakes_reset(void *unused) { g_nfakes = 0; }

static FakeCom *fake_new(UINT64 w, UINT h, DXGI_FORMAT fmt)
{
    if (!g_fakeVtbl[0]) {
        for (int i = 0; i < FAKE_SLOTS; i++) g_fakeVtbl[i] = (void *)fake_trap;
        g_fakeVtbl[0] = (void *)fake_qi;
        g_fakeVtbl[1] = (void *)fake_addref;
        g_fakeVtbl[2] = (void *)fake_release;
        g_fakeVtbl[offsetof(ID3D12ResourceVtbl, GetDesc) / sizeof(void *)] = (void *)fake_getdesc;
    }
    if (g_nfakes == 0) tt_defer(fakes_reset, NULL);
    REQUIRE(g_nfakes < 16);
    FakeCom *f = calloc(1, sizeof(FakeCom));
    tt_defer(sdk_free_deferred, f);
    f->vtbl = g_fakeVtbl;
    f->refs = 1;
    f->desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    f->desc.Width = w;
    f->desc.Height = h;
    f->desc.DepthOrArraySize = 1;
    f->desc.MipLevels = 1;
    f->desc.Format = fmt;
    f->desc.SampleDesc.Count = 1;
    g_fakes[g_nfakes++] = f;
    return f;
}

// ===================================================================================== exports & PE

TEST(exports_are_present_and_undecorated)
{
    Sdk *s = sdk_loaded(NULL);
    CHECK(GetProcAddress(s->h, "create_odyssey3d_factory_instance") != NULL);
    CHECK(GetProcAddress(s->h, "destroy_odyssey3d_factory_instance") != NULL);
    CHECK(GetProcAddress(s->h, "braindance3d_version") != NULL);
}

TEST(version_export_matches_version_h)
{
    Sdk *s = sdk_loaded(NULL);
    REQUIRE(s->version != NULL);
    const char *v = s->version();
    CHECK_EQ_STR(v, BD3D_VERSION_STR);
    CHECK_EQ_PTR(s->version(), v); // a static string, stable across calls
}

TEST(version_resource_matches_version_h_and_installer_product_name)
{
    tu_require_dll();
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(tu_dll_path(), &dummy);
    REQUIRE_MSG(size > 0, "no VERSIONINFO resource");
    void *buf = malloc(size);
    tt_defer(sdk_free_deferred, buf);
    REQUIRE(GetFileVersionInfoW(tu_dll_path(), 0, size, buf));
    VS_FIXEDFILEINFO *ffi = NULL;
    UINT len = 0;
    REQUIRE(VerQueryValueW(buf, L"\\", (void **)&ffi, &len) && ffi);
    CHECK_EQ_INT(HIWORD(ffi->dwFileVersionMS), BD3D_VERSION_MAJOR);
    CHECK_EQ_INT(LOWORD(ffi->dwFileVersionMS), BD3D_VERSION_MINOR);
    CHECK_EQ_INT(HIWORD(ffi->dwFileVersionLS), BD3D_VERSION_PATCH);
    CHECK_EQ_INT(HIWORD(ffi->dwProductVersionMS), BD3D_VERSION_MAJOR);
    CHECK_EQ_INT(LOWORD(ffi->dwProductVersionMS), BD3D_VERSION_MINOR);
    struct { const wchar_t *key; const wchar_t *want; } kv[] = {
        // The installer recognises its own DLL by this exact ProductName (Braindance3D-Setup.ps1, $script:ProductName);
        // the Pester tests check that the script and src/version.rc agree.
        { L"\\StringFileInfo\\040904b0\\ProductName", L"" BD3D_PRODUCT_NAME },
        { L"\\StringFileInfo\\040904b0\\ProductVersion", L"" BD3D_VERSION_STR },
        { L"\\StringFileInfo\\040904b0\\FileVersion", L"" BD3D_VERSION_STR },
        { L"\\StringFileInfo\\040904b0\\OriginalFilename", L"Odyssey3DSDK.dll" },
    };
    for (size_t i = 0; i < sizeof(kv) / sizeof(kv[0]); i++) {
        wchar_t *val = NULL;
        UINT n = 0;
        CHECK_MSG(VerQueryValueW(buf, kv[i].key, (void **)&val, &n) && val, "missing %ls", kv[i].key);
        if (val) CHECK_EQ_WSTR(val, kv[i].want);
    }
}

TEST(pe_is_x64_dll_with_aslr_nx_and_high_entropy_va)
{
    Sdk *s = sdk_loaded(NULL);
    IMAGE_NT_HEADERS64 *nt = tu_nt(s->h);
    CHECK_EQ_INT(nt->FileHeader.Machine, IMAGE_FILE_MACHINE_AMD64);
    CHECK(nt->FileHeader.Characteristics & IMAGE_FILE_DLL);
    WORD dc = nt->OptionalHeader.DllCharacteristics;
    CHECK(dc & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE);
    CHECK(dc & IMAGE_DLLCHARACTERISTICS_NX_COMPAT);
    CHECK(dc & IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA);
}

// The DLL must load on a player's PC without the MinGW runtime (libgcc_s, libwinpthread, ...).
TEST(pe_imports_only_windows_system_dlls)
{
    Sdk *s = sdk_loaded(NULL);
    IMAGE_NT_HEADERS64 *nt = tu_nt(s->h);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    REQUIRE(dir.VirtualAddress != 0);
    IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)((BYTE *)s->h + dir.VirtualAddress);
    static const char *allowed[] = { "kernel32.dll", "user32.dll", "msvcrt.dll", "ntdll.dll", "advapi32.dll",
                                     "ucrtbase.dll", "gdi32.dll", "shell32.dll", "version.dll" };
    int n = 0;
    for (; imp->Name; imp++, n++) {
        const char *name = (const char *)((BYTE *)s->h + imp->Name);
        int ok = !_strnicmp(name, "api-ms-win-", 11);
        for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
            if (!_stricmp(name, allowed[i])) ok = 1;
        CHECK_MSG(ok, "imports %s, which is not part of Windows", name);
    }
    CHECK(n > 0);
}

// ===================================================================================== factory

TEST(factory_vtable_has_32_slots_pointing_into_the_dll)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *f = sdk_factory(s);
    CHECK(tu_in_image(s->h, f));
    for (int i = 0; i < NSLOTS; i++) CHECK_MSG(tu_in_code(s->h, f->vtbl[i]), "factory slot %d = %p", i, f->vtbl[i]);
}

TEST(factory_destroy_is_safe_and_factory_can_be_created_again)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *f = sdk_factory(s);
    s->destroy(f);
    s->destroy(NULL);
    Obj *f2 = sdk_factory(s);
    Obj *dev = NULL;
    SLOT(f2, FAC_CREATE_DEVICE, PFN_CreateDevice)(f2, &dev);
    REQUIRE(dev != NULL);
    CHECK_EQ_INT(SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev), 1);
    device_release(dev);
    s->destroy(f2);
}

TEST(factory_create_device_writes_out_and_returns_it)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *f = sdk_factory(s);
    Obj *a = (Obj *)(uintptr_t)0xdeadbeef, *b = NULL;
    CHECK_EQ_PTR(SLOT(f, FAC_CREATE_DEVICE, PFN_CreateDevice)(f, &a), &a);
    CHECK_EQ_PTR(SLOT(f, FAC_CREATE_DEVICE, PFN_CreateDevice)(f, &b), &b);
    REQUIRE(a != NULL && a != (Obj *)(uintptr_t)0xdeadbeef);
    REQUIRE(b != NULL);
    CHECK(a != b);
    for (int i = 0; i < NSLOTS; i++) CHECK_MSG(tu_in_code(s->h, a->vtbl[i]), "device slot %d = %p", i, a->vtbl[i]);
    device_release(a);
    device_release(b);
}

TEST(factory_unmapped_slots_return_their_first_argument)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *f = sdk_factory(s);
    for (int i = 0; i < NSLOTS; i++) {
        if (is_known(kFacKnown, 2, i)) continue;
        uintptr_t a = 0x1000 + (uintptr_t)i, r = SLOT(f, i, PFN_Generic)(f, a, 2, 3, 4, 5);
        CHECK_MSG(r == a, "factory slot %d returned 0x%llx, expected 0x%llx", i, (unsigned long long)r,
                  (unsigned long long)a);
        CHECK_EQ_INT(SLOT(f, i, PFN_Generic)(f, 0, 0, 0, 0, 0), 0);
    }
}

// ===================================================================================== device

TEST(device_eye_positions_are_64mm_ipd_centred_600mm_away)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    float l[3] = { -1, -1, -1 }, r[3] = { -1, -1, -1 };
    CHECK_EQ_PTR(SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, l, 0), l);
    CHECK_EQ_PTR(SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, r, 1), r);
    CHECK_NEAR(l[0], -32.0, 1e-6);
    CHECK_NEAR(l[1], 0.0, 1e-6);
    CHECK_NEAR(l[2], 600.0, 1e-6);
    CHECK_NEAR(r[0], 32.0, 1e-6);
    CHECK_NEAR(r[1], 0.0, 1e-6);
    CHECK_NEAR(r[2], 600.0, 1e-6);
    CHECK_NEAR(r[0] - l[0], 64.0, 1e-6);
    // Repeated calls give the same answer (the game polls ~110 times a second).
    for (int i = 0; i < 1000; i++) {
        float e[3];
        SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, e, i & 1);
        if (e[0] != ((i & 1) ? 32.0f : -32.0f) || e[1] != 0.0f || e[2] != 600.0f) {
            CHECK_MSG(0, "call %d: eye %d = %g %g %g", i, i & 1, e[0], e[1], e[2]);
            break;
        }
    }
    device_release(dev);
}

TEST(device_eye_position_writes_exactly_three_floats)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    float buf[6] = { 7, 7, 7, 7, 7, 7 };
    SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, buf + 1, 0);
    CHECK(buf[0] == 7.0f);
    CHECK(buf[4] == 7.0f);
    CHECK(buf[5] == 7.0f);
    device_release(dev);
}

TEST(device_float2_18_is_10000)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    float v[4] = { 0, 0, 7, 7 };
    CHECK_EQ_PTR(SLOT(dev, DEV_GET_FLOAT2_18, PFN_GetFloat2)(dev, v), v);
    CHECK_NEAR(v[0], 10000.0, 1e-3);
    CHECK_NEAR(v[1], 10000.0, 1e-3);
    CHECK(v[2] == 7.0f && v[3] == 7.0f);
    CHECK_NEAR(v[0] * 0.01, 100.0, 1e-6); // the game's default after scaling
    device_release(dev);
}

TEST(device_resolution_and_rect_match_primary_monitor_without_a_window)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    RECT want = monitor_rect_of(NULL);
    REQUIRE(want.right > want.left && want.bottom > want.top);
    int64_t rect[5] = { -1, -1, -1, -1, 0x7777 };
    CHECK_EQ_PTR(SLOT(dev, DEV_GET_MONITOR_RECT, PFN_GetRect)(dev, rect), rect);
    CHECK_MSG(rect_matches(rect, want), "rect {top,left,right,bottom} = {%lld,%lld,%lld,%lld}, monitor l=%ld t=%ld r=%ld b=%ld",
              (long long)rect[0], (long long)rect[1], (long long)rect[2], (long long)rect[3], want.left, want.top,
              want.right, want.bottom);
    CHECK_EQ_INT(rect[4], 0x7777);
    float res[3] = { 0, 0, 7 };
    CHECK_EQ_PTR(SLOT(dev, DEV_GET_RESOLUTION, PFN_GetFloat2)(dev, res), res);
    CHECK_EQ_INT((int)res[0], want.right - want.left);
    CHECK_EQ_INT((int)res[1], want.bottom - want.top);
    CHECK(res[0] == (float)(int)res[0] && res[1] == (float)(int)res[1]); // whole pixels
    CHECK(res[2] == 7.0f);
    // Resolution and rectangle agree.
    CHECK_EQ_INT((int64_t)res[0], rect[2] - rect[1]);
    CHECK_EQ_INT((int64_t)res[1], rect[3] - rect[0]);
    tt_note("monitor %dx%d at %ld,%ld", (int)res[0], (int)res[1], want.left, want.top);
    device_release(dev);
}

static BOOL CALLBACK collect_monitors(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    RECT *list = (RECT *)lp;
    int n = (int)list[0].left;
    if (n < 15) {
        list[1 + n] = *r;
        list[0].left = n + 1;
    }
    return TRUE;
}

TEST(device_follows_the_game_windows_monitor)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    RECT mons[16] = {{0}};
    EnumDisplayMonitors(NULL, NULL, collect_monitors, (LPARAM)mons);
    int n = (int)mons[0].left;
    REQUIRE(n >= 1);
    HWND w = tu_window(L"Cyberpunk 2077 (C) 2020 by CD Projekt RED", mons[1].left + 10, mons[1].top + 10, 320, 200);
    for (int i = 0; i < n; i++) {
        RECT m = mons[1 + i];
        SetWindowPos(w, NULL, m.left + 20, m.top + 20, 400, 300, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        tu_pump();
        RECT want = monitor_rect_of(w);
        int64_t got[4];
        CHECK_MSG(wait_for_monitor(dev, want, got, 3000),
                  "monitor %d: got {top,left,right,bottom} = {%lld,%lld,%lld,%lld}, want l=%ld t=%ld r=%ld b=%ld", i,
                  (long long)got[0], (long long)got[1], (long long)got[2], (long long)got[3], want.left, want.top,
                  want.right, want.bottom);
        float res[2];
        SLOT(dev, DEV_GET_RESOLUTION, PFN_GetFloat2)(dev, res);
        CHECK_EQ_INT((int)res[0], want.right - want.left);
        CHECK_EQ_INT((int)res[1], want.bottom - want.top);
    }
    if (n < 2) tt_note("only one monitor: could not check that the window's monitor wins over the primary");
    device_release(dev);
}

TEST(device_picks_the_largest_window_when_several_are_open)
{
    RECT mons[16] = {{0}};
    EnumDisplayMonitors(NULL, NULL, collect_monitors, (LPARAM)mons);
    if (mons[0].left < 2) SKIP("needs two monitors");
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    RECT a = mons[1], b = mons[2];
    tu_window(L"small", a.left + 10, a.top + 10, 100, 80);
    HWND big = tu_window(L"big", b.left + 10, b.top + 10, 600, 400);
    int64_t got[4];
    CHECK(wait_for_monitor(dev, monitor_rect_of(big), got, 3000));
    device_release(dev);
}

TEST(device_is_available)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    for (int i = 0; i < 100; i++) {
        unsigned char r = SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev);
        if (r != 1) {
            CHECK_EQ_INT(r, 1);
            break;
        }
    }
    device_release(dev);
}

TEST(device_slot9_hint_returns_zero)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    CHECK_EQ_INT(SLOT(dev, DEV_HINT48, PFN_Hint)(dev, 0), 0);
    CHECK_EQ_INT(SLOT(dev, DEV_HINT48, PFN_Hint)(dev, 1), 0);
    CHECK_EQ_INT(SLOT(dev, DEV_HINT48, PFN_Hint)(dev, 0xffffffffffffffffull), 0);
    device_release(dev);
}

TEST(device_unmapped_slots_return_their_first_argument)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    for (int i = 0; i < NSLOTS; i++) {
        if (is_known(kDevKnown, 7, i)) continue;
        uintptr_t a = 0xabcd0000 + (uintptr_t)i;
        uintptr_t r = SLOT(dev, i, PFN_Generic)(dev, a, 0x11, 0x22, 0x33, 0x44);
        CHECK_MSG(r == a, "device slot %d returned 0x%llx", i, (unsigned long long)r);
        // An out-param getter called through an unmapped slot gets its out pointer back untouched.
        int64_t out[4] = { 5, 6, 7, 8 };
        CHECK_EQ_PTR(SLOT(dev, i, PFN_Generic)(dev, (uintptr_t)out, 0, 0, 0, 0), out);
        CHECK(out[0] == 5 && out[3] == 8);
    }
    device_release(dev);
}

TEST(device_destroy_flags0_keeps_the_object_flags1_frees_it)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    CHECK_EQ_PTR(SLOT(dev, DEV_DESTROY, PFN_ScalarDtor)(dev, 0), dev);
    // Still usable after a destructor call without the "free" flag.
    CHECK_EQ_INT(SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev), 1);
    float e[3];
    SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, e, 1);
    CHECK_NEAR(e[0], 32.0, 1e-6);
    CHECK_EQ_PTR(SLOT(dev, DEV_DESTROY, PFN_ScalarDtor)(dev, 1), dev);
}

TEST(device_create_destroy_does_not_leak)
{
    Sdk *s = sdk_loaded("[Log]\nLevel=0\n");
    Obj *f = sdk_factory(s);
    for (int i = 0; i < 1000; i++) { // warm up the heap
        Obj *d = NULL;
        SLOT(f, FAC_CREATE_DEVICE, PFN_CreateDevice)(f, &d);
        device_release(d);
    }
    long long before = tu_heap_busy_bytes();
    if (before < 0) SKIP("HeapWalk is not available");
    const int N = 50000;
    for (int i = 0; i < N; i++) {
        Obj *d = NULL;
        SLOT(f, FAC_CREATE_DEVICE, PFN_CreateDevice)(f, &d);
        REQUIRE(d != NULL);
        device_release(d);
    }
    long long after = tu_heap_busy_bytes();
    CHECK_MSG(after - before < 64 * 1024, "process heap grew by %lld bytes over %d device create/destroy cycles",
              after - before, N);
}

// ===================================================================================== weaver

TEST(create_weaver_returns_msvc_shared_ptr)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *f = sdk_factory(s);
    Obj *dev = sdk_device(s);
    SharedPtr sp = { (Obj *)(uintptr_t)1, (RefCount *)(uintptr_t)1 };
    SharedPtr *r = SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, dev, NULL, NULL, NULL, 0, 0, NULL);
    CHECK_EQ_PTR(r, &sp);
    REQUIRE(sp.ptr != NULL && sp.ptr != (Obj *)(uintptr_t)1);
    REQUIRE(sp.rc != NULL && sp.rc != (RefCount *)(uintptr_t)1);
    for (int i = 0; i < NSLOTS; i++) CHECK_MSG(tu_in_code(s->h, sp.ptr->vtbl[i]), "weaver slot %d", i);
    // _Ref_count_base: vtable at +0, _Uses (long) at +8, _Weaks (long) at +0xC, both 1.
    BYTE *rc = (BYTE *)sp.rc;
    CHECK_EQ_INT(*(LONG *)(rc + 8), 1);
    CHECK_EQ_INT(*(LONG *)(rc + 12), 1);
    CHECK(tu_in_code(s->h, sp.rc->vtbl[0])); // _Destroy
    CHECK(tu_in_code(s->h, sp.rc->vtbl[1])); // _Delete_this
    CHECK(sp.rc->vtbl[0] != sp.rc->vtbl[1]);
    sp_release(&sp);
    device_release(dev);
}

TEST(weaver_shared_ptr_release_with_a_weak_reference_outstanding)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *f = sdk_factory(s);
    SharedPtr sp = { 0 };
    SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL);
    REQUIRE(sp.rc != NULL);
    RefCount *rc = sp.rc;
    InterlockedIncrement(&rc->weaks); // a weak_ptr
    InterlockedIncrement(&rc->uses);  // a second shared_ptr copy
    // Drop one strong ref: nothing is destroyed.
    CHECK_EQ_INT(InterlockedDecrement(&rc->uses), 1);
    // Last strong ref: _Destroy() frees the weaver; the control block must stay valid for the weak ref.
    CHECK_EQ_INT(InterlockedDecrement(&rc->uses), 0);
    ((PFN_RcVoid)rc->vtbl[0])(rc);
    CHECK_EQ_INT(InterlockedDecrement(&rc->weaks), 1);
    CHECK_EQ_INT(rc->uses, 0); // still readable
    CHECK_EQ_INT(rc->weaks, 1);
    // Weak ref gone: _Delete_this() frees the control block.
    CHECK_EQ_INT(InterlockedDecrement(&rc->weaks), 0);
    ((PFN_RcVoid)rc->vtbl[1])(rc);
}

TEST(create_weaver_with_game_like_arguments_and_window)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *f = sdk_factory(s);
    Obj *dev = sdk_device(s);
    HWND hwnd = tu_window(L"Cyberpunk 2077 (C) 2020 by CD Projekt RED", 0, 0, 640, 360);
    FakeCom *d3d = fake_new(0, 0, 0), *alloc = fake_new(0, 0, 0), *queue = fake_new(0, 0, 0);
    SharedPtr sp = { 0 };
    SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, dev, d3d, alloc, queue, 0, 0, hwnd);
    REQUIRE(sp.ptr != NULL);
    REQUIRE(sp.rc != NULL);
    sp_release(&sp);
    // Whatever the weaver did with the D3D objects, references must be balanced once it is gone.
    CHECK_EQ_INT(d3d->refs, 1);
    CHECK_EQ_INT(alloc->refs, 1);
    CHECK_EQ_INT(queue->refs, 1);
    // A destroyed window handle must not crash it either.
    HWND gone = CreateWindowExW(0, L"STATIC", L"x", WS_POPUP, 0, 0, 10, 10, NULL, NULL, NULL, NULL);
    DestroyWindow(gone);
    SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, dev, NULL, NULL, NULL, 0, 0, gone);
    sp_release(&sp);
    device_release(dev);
}

static void exercise_weaver_null_inputs(const char *ini)
{
    Sdk *s = sdk_loaded(ini);
    Obj *f = sdk_factory(s);
    SharedPtr sp = { 0 };
    SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL);
    REQUIRE(sp.ptr != NULL);
    Obj *w = sp.ptr;
    for (int i = 0; i < 300; i++) {
        SLOT(w, WV_SET_INPUT, PFN_SetInput)(w, NULL, 28);
        SLOT(w, WV_SET_OUTPUT, PFN_SetPtr)(w, NULL);
        SLOT(w, WV_SET_CMDLIST, PFN_SetPtr)(w, NULL);
        SLOT(w, WV_WEAVE, PFN_Weave)(w, 0, 0, 0, 0);
        SLOT(w, WV_WEAVE, PFN_Weave)(w, 3840, 2160, -5, 7);
    }
    sp_release(&sp);
}

TEST(weaver_slots_accept_null_inputs_log_level_0) { exercise_weaver_null_inputs("[Log]\nLevel=0\n"); }
TEST(weaver_slots_accept_null_inputs_log_level_1) { exercise_weaver_null_inputs("[Log]\nLevel=1\n"); }
TEST(weaver_slots_accept_null_inputs_log_level_2) { exercise_weaver_null_inputs("[Log]\nLevel=2\n"); }

static void exercise_weaver_fake_resources(const char *ini)
{
    Sdk *s = sdk_loaded(ini);
    Obj *f = sdk_factory(s);
    SharedPtr sp = { 0 };
    SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL);
    REQUIRE(sp.ptr != NULL);
    Obj *w = sp.ptr;
    FakeCom *in1 = fake_new(3840, 1080, DXGI_FORMAT_R10G10B10A2_UNORM);
    FakeCom *in2 = fake_new(5120, 1440, DXGI_FORMAT_R16G16B16A16_FLOAT);
    FakeCom *out = fake_new(1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM);
    FakeCom *cl = fake_new(0, 0, 0);
    LONG traps0 = g_trapCalls;
    for (int i = 0; i < 300; i++) {
        FakeCom *in = (i / 100) == 1 ? in2 : in1; // the input size changes twice
        SLOT(w, WV_SET_INPUT, PFN_SetInput)(w, in, (int)in->desc.Format);
        SLOT(w, WV_SET_OUTPUT, PFN_SetPtr)(w, out);
        SLOT(w, WV_SET_CMDLIST, PFN_SetPtr)(w, cl);
        SLOT(w, WV_WEAVE, PFN_Weave)(w, 1920, 1080, 0, 0);
    }
    // ID3D12Resource::GetDesc must be called the C++ way (this in RCX, return buffer in RDX).
    CHECK_EQ_INT(in1->getdesc_bad_this, 0);
    tt_note("GetDesc calls: in1=%ld in2=%ld out=%ld", in1->getdesc_calls, in2->getdesc_calls, out->getdesc_calls);
    sp_release(&sp);
    CHECK_EQ_INT(in1->refs, 1);
    CHECK_EQ_INT(in2->refs, 1);
    CHECK_EQ_INT(out->refs, 1);
    CHECK_EQ_INT(cl->refs, 1);
    // The weaver is a no-op: it records nothing into the command list.
    CHECK_EQ_INT(g_trapCalls - traps0, 0);
}

TEST(weaver_slots_with_fake_d3d12_resources_log_level_0) { exercise_weaver_fake_resources("[Log]\nLevel=0\n"); }
TEST(weaver_slots_with_fake_d3d12_resources_log_level_1) { exercise_weaver_fake_resources("[Log]\nLevel=1\n"); }
TEST(weaver_slots_with_fake_d3d12_resources_log_level_2) { exercise_weaver_fake_resources("[Log]\nLevel=2\n"); }

TEST(weaver_unmapped_slots_return_their_first_argument)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *f = sdk_factory(s);
    SharedPtr sp = { 0 };
    SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL);
    REQUIRE(sp.ptr != NULL);
    for (int i = 0; i < NSLOTS; i++) {
        if (is_known(kWvKnown, 4, i)) continue;
        uintptr_t a = 0x5eed0000 + (uintptr_t)i;
        CHECK_MSG(SLOT(sp.ptr, i, PFN_Generic)(sp.ptr, a, 9, 9, 9, 9) == a, "weaver slot %d", i);
    }
    sp_release(&sp);
}

TEST(weaver_create_release_does_not_leak)
{
    Sdk *s = sdk_loaded("[Log]\nLevel=0\n");
    Obj *f = sdk_factory(s);
    for (int i = 0; i < 500; i++) {
        SharedPtr sp = { 0 };
        SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL);
        sp_release(&sp);
    }
    long long before = tu_heap_busy_bytes();
    if (before < 0) SKIP("HeapWalk is not available");
    const int N = 20000;
    for (int i = 0; i < N; i++) {
        SharedPtr sp = { 0 };
        SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL);
        REQUIRE(sp.rc != NULL);
        sp_release(&sp);
    }
    long long after = tu_heap_busy_bytes();
    CHECK_MSG(after - before < 64 * 1024, "process heap grew by %lld bytes over %d weaver create/release cycles",
              after - before, N);
}

// ===================================================================================== the game's sequence

TEST(game_startup_and_frame_loop_sequence)
{
    Sdk *s = sdk_prepare(NULL);
    HWND hwnd = tu_window(L"Cyberpunk 2077 (C) 2020 by CD Projekt RED", 0, 0, 800, 450);
    sdk_load(s);
    Obj *f = s->create();
    REQUIRE(f != NULL);
    Obj *dev = NULL;
    SLOT(f, FAC_CREATE_DEVICE, PFN_CreateDevice)(f, &dev);
    REQUIRE(dev != NULL);
    REQUIRE(SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev));
    float res[2];
    SLOT(dev, DEV_GET_RESOLUTION, PFN_GetFloat2)(dev, res);
    int64_t rect[4];
    SLOT(dev, DEV_GET_MONITOR_RECT, PFN_GetRect)(dev, rect);
    // The game compares the rectangle against the DXGI output its window is on.
    RECT want = monitor_rect_of(hwnd);
    CHECK(wait_for_monitor(dev, want, rect, 3000));
    float f18[2];
    SLOT(dev, DEV_GET_FLOAT2_18, PFN_GetFloat2)(dev, f18);
    for (int frame = 0; frame < 330; frame++) { // ~3 s of polling at the game's rate
        float l[3], r[3];
        REQUIRE(SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev));
        SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, l, 0);
        SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, r, 1);
        REQUIRE(l[0] < r[0]);
        for (int k = 0; k < 6; k++) CHECK_EQ_INT(SLOT(dev, DEV_HINT48, PFN_Hint)(dev, 0), 0);
        if (frame % 50 == 0) {
            SLOT(dev, DEV_GET_RESOLUTION, PFN_GetFloat2)(dev, res);
            CHECK_EQ_INT((int)res[0], want.right - want.left);
        }
    }
    CHECK_EQ_PTR(SLOT(dev, DEV_DESTROY, PFN_ScalarDtor)(dev, 1), dev);
    s->destroy(f);
    sdk_unload(s);
    CHECK(GetModuleHandleW(s->dll) == NULL);
    // The log was written and released.
    CHECK(tu_file_size(s->log) > 0);
    CHECK_MSG(DeleteFileW(s->log), "log still locked after unload (%lu)", GetLastError());
}

// ===================================================================================== concurrency

typedef struct {
    Obj *dev;
    DWORD ms;
    volatile LONG *errors;
    volatile LONG64 *calls;
    HANDLE go;
} HammerArgs;

static DWORD WINAPI hammer(LPVOID p)
{
    HammerArgs *a = p;
    Obj *dev = a->dev;
    WaitForSingleObject(a->go, INFINITE);
    DWORD t0 = GetTickCount();
    LONG64 n = 0;
    int64_t rect0[4];
    SLOT(dev, DEV_GET_MONITOR_RECT, PFN_GetRect)(dev, rect0);
    while (GetTickCount() - t0 < a->ms) {
        for (int k = 0; k < 64; k++, n++) {
            float e[3];
            int eye = (int)(n & 1);
            SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, e, eye);
            if (e[0] != (eye ? 32.0f : -32.0f) || e[1] != 0.0f || e[2] != 600.0f) InterlockedIncrement(a->errors);
            if (SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev) != 1) InterlockedIncrement(a->errors);
            if (SLOT(dev, DEV_HINT48, PFN_Hint)(dev, 0) != 0) InterlockedIncrement(a->errors);
            if ((n & 7) == 0) {
                float res[2];
                int64_t rect[4];
                SLOT(dev, DEV_GET_RESOLUTION, PFN_GetFloat2)(dev, res);
                SLOT(dev, DEV_GET_MONITOR_RECT, PFN_GetRect)(dev, rect);
                if (memcmp(rect, rect0, sizeof(rect)) != 0) InterlockedIncrement(a->errors);
                if ((int64_t)res[0] != rect[2] - rect[1] || (int64_t)res[1] != rect[3] - rect[0])
                    InterlockedIncrement(a->errors);
                float f[2];
                SLOT(dev, DEV_GET_FLOAT2_18, PFN_GetFloat2)(dev, f);
                if (f[0] != 10000.0f) InterlockedIncrement(a->errors);
            }
            if ((n & 15) == 0 && SLOT(dev, 5, PFN_Generic)(dev, (uintptr_t)n, 0, 0, 0, 0) != (uintptr_t)n)
                InterlockedIncrement(a->errors);
        }
    }
    InterlockedAdd64(a->calls, n);
    return 0;
}

static void hammer_hot_slots(const char *ini, int nthreads, DWORD ms)
{
    Sdk *s = sdk_loaded(ini);
    Obj *dev = sdk_device(s);
    volatile LONG errors = 0;
    volatile LONG64 calls = 0;
    HANDLE go = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th[64];
    HammerArgs args = { dev, ms, &errors, &calls, go };
    for (int i = 0; i < nthreads; i++) {
        th[i] = CreateThread(NULL, 0, hammer, &args, 0, NULL);
        REQUIRE(th[i] != NULL);
    }
    SetEvent(go);
    DWORD w = WaitForMultipleObjects(nthreads, th, TRUE, ms + 60000);
    REQUIRE_MSG(w < WAIT_OBJECT_0 + nthreads, "worker threads did not finish (deadlock?)");
    for (int i = 0; i < nthreads; i++) CloseHandle(th[i]);
    CloseHandle(go);
    CHECK_EQ_INT(errors, 0);
    tt_note("%d threads, %lld iterations in %lu ms", nthreads, (long long)calls, ms);
    device_release(dev);
    sdk_unload(s);
    if (tu_exists(s->log)) {
        long long size = tu_file_size(s->log);
        // The log stays small however hard the game polls: ~1 line per slot per interval after a fixed budget.
        CHECK_MSG(size < 2 * 1024 * 1024, "log grew to %lld bytes", size);
        tt_note("log size %lld bytes", size);
    }
}

TEST(hot_slots_from_many_threads_log_level_0) { hammer_hot_slots("[Log]\nLevel=0\n", 16, 1500); }
TEST(hot_slots_from_many_threads_log_level_1) { hammer_hot_slots("[Log]\nLevel=1\n", 16, 1500); }
TEST(hot_slots_from_many_threads_log_level_2) { hammer_hot_slots("[Log]\nLevel=2\n", 16, 2500); }

typedef struct { Obj *f; volatile LONG *errors; } ChurnArgs;

static DWORD WINAPI churn(LPVOID p)
{
    ChurnArgs *a = p;
    for (int i = 0; i < 2000; i++) {
        Obj *d = NULL;
        SLOT(a->f, FAC_CREATE_DEVICE, PFN_CreateDevice)(a->f, &d);
        if (!d || SLOT(d, DEV_IS_AVAILABLE, PFN_IsAvailable)(d) != 1) {
            InterlockedIncrement(a->errors);
            continue;
        }
        SharedPtr sp = { 0 };
        SLOT(a->f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(a->f, &sp, d, NULL, NULL, NULL, 0, 0, NULL);
        if (!sp.ptr) InterlockedIncrement(a->errors);
        else SLOT(sp.ptr, WV_WEAVE, PFN_Weave)(sp.ptr, 1, 1, 0, 0);
        sp_release(&sp);
        device_release(d);
    }
    return 0;
}

TEST(devices_and_weavers_created_and_destroyed_from_many_threads)
{
    Sdk *s = sdk_loaded("[Log]\nLevel=2\n");
    Obj *f = sdk_factory(s);
    volatile LONG errors = 0;
    ChurnArgs a = { f, &errors };
    HANDLE th[8];
    for (int i = 0; i < 8; i++) th[i] = CreateThread(NULL, 0, churn, &a, 0, NULL);
    REQUIRE(WaitForMultipleObjects(8, th, TRUE, 120000) < WAIT_OBJECT_0 + 8);
    for (int i = 0; i < 8; i++) CloseHandle(th[i]);
    CHECK_EQ_INT(errors, 0);
}

// ===================================================================================== logging & settings

static int log_lines(const wchar_t *path)
{
    char *t = tu_read_file(path, NULL);
    if (!t) return -1;
    int n = tu_count_lines(t);
    free(t);
    return n;
}

TEST(log_is_created_in_the_install_folder_with_default_level)
{
    Sdk *s = sdk_loaded(NULL); // no settings file: level 1
    CHECK_MSG(tu_exists(s->log), "missing %ls", s->log);
    wchar_t p[TU_PATH];
    tu_join(p, TU_PATH, s->core, L"braindance3d.log");
    CHECK(!tu_exists(p));
    sdk_unload(s);
    char *t = tu_read_file(s->log, NULL);
    REQUIRE(t != NULL);
    tt_defer(sdk_free_deferred, t);
    CHECK(strlen(t) > 0);
    CHECK_MSG(strstr(t, BD3D_VERSION_STR) != NULL, "the log does not name the version " BD3D_VERSION_STR);
}

TEST(log_with_the_shipped_settings_file)
{
    // package/bin/x64/Braindance3D/Braindance3D-Settings.ini, found relative to the DLL under test (<repo>\build\).
    wchar_t ini[TU_PATH];
    _snwprintf(ini, TU_PATH, L"%ls\\..\\..\\package\\bin\\x64\\Braindance3D\\" SETTINGS_FILE, tu_dll_path());
    ini[TU_PATH - 1] = 0;
    char *text = tu_read_file(ini, NULL);
    if (!text) SKIP("shipped settings file not found next to the DLL under test (%ls)", ini);
    tt_defer(sdk_free_deferred, text);
    Sdk *s = sdk_loaded(text);
    CHECK(tu_exists(s->log)); // ships with Level = 1
    Obj *dev = sdk_device(s);
    CHECK_EQ_INT(SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev), 1);
    device_release(dev);
}

TEST(log_level_0_creates_no_file)
{
    Sdk *s = sdk_loaded("[Log]\r\nLevel = 0\r\n");
    Obj *dev = sdk_device(s);
    for (int i = 0; i < 100; i++) SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev);
    device_release(dev);
    CHECK(!tu_exists(s->log));
    CHECK(!tu_exists(s->oldlog));
    wchar_t lad[TU_PATH] = L"", p[TU_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", lad, TU_PATH);
    tu_join(p, TU_PATH, lad, L"Braindance3D\\braindance3d.log");
    CHECK(!tu_exists(p));
    sdk_unload(s);
    CHECK(!tu_exists(s->log));
}

TEST(log_level_that_is_not_a_number_means_normal)
{
    const char *inis[] = { "[Log]\nLevel=-1\n", "[Log]\nLevel=abc\n", "[Log]\nLevel=\n", "[Log]\nLevel=off\n" };
    for (size_t i = 0; i < sizeof(inis) / sizeof(inis[0]); i++) {
        Sdk *s = sdk_loaded(inis[i]);
        CHECK_MSG(tu_exists(s->log), "no log with settings \"%s\"", inis[i]);
        sdk_unload(s);
    }
}

TEST(log_level_above_2_is_treated_as_verbose)
{
    // Same workload at levels 1, 2 and 7: 7 must log like 2 (more than 1).
    const char *inis[3] = { "[Log]\nLevel=1\n", "[Log]\nLevel=2\n", "[Log]\nLevel=7\n" };
    int lines[3];
    for (int k = 0; k < 3; k++) {
        Sdk *s = sdk_loaded(inis[k]);
        Obj *dev = sdk_device(s);
        for (int i = 0; i < 200; i++) {
            float e[3];
            SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, e, i & 1);
        }
        device_release(dev);
        sdk_unload(s);
        lines[k] = log_lines(s->log);
    }
    tt_note("log lines: level1=%d level2=%d level7=%d", lines[0], lines[1], lines[2]);
    CHECK(lines[0] > 0);
    CHECK(lines[1] > lines[0] + 50);
    CHECK(lines[2] > lines[0] + 50);
    CHECK(abs(lines[2] - lines[1]) <= 5);
}

TEST(old_settings_file_name_is_not_read)
{
    // 0.1.0 drafts used Braindance3D.ini; only Braindance3D-Settings.ini counts now.
    Sdk *s = sdk_prepare(NULL);
    wchar_t p[TU_PATH];
    tu_join(p, TU_PATH, s->root, L"Braindance3D.ini");
    REQUIRE(tu_write_text(p, "[Log]\nLevel=0\n"));
    sdk_load(s);
    CHECK(tu_exists(s->log));
}

TEST(log_is_bounded_under_heavy_polling)
{
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    for (int i = 0; i < 200000; i++) {
        float e[3];
        SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, e, i & 1);
        SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev);
        SLOT(dev, DEV_HINT48, PFN_Hint)(dev, 0);
    }
    device_release(dev);
    sdk_unload(s);
    long long size = tu_file_size(s->log);
    CHECK(size > 0);
    CHECK_MSG(size < 64 * 1024, "600000 calls at level 1 produced a %lld byte log", size);
}

static int count_logs(const wchar_t *dir)
{
    int n = 0;
    wchar_t pat[TU_PATH];
    tu_join(pat, TU_PATH, dir, L"*.log");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do n++;
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}

TEST(log_rotation_keeps_exactly_one_previous_session)
{
    Sdk *s = sdk_prepare(NULL);
    REQUIRE(tu_write_text(s->log, "PREVIOUS-SESSION-MARKER\n"));
    REQUIRE(tu_write_text(s->oldlog, "OLDER-SESSION-MARKER\n"));
    sdk_load(s);
    sdk_unload(s);
    char *cur = tu_read_file(s->log, NULL), *old = tu_read_file(s->oldlog, NULL);
    REQUIRE(cur && old);
    tt_defer(sdk_free_deferred, cur);
    tt_defer(sdk_free_deferred, old);
    CHECK(strstr(old, "PREVIOUS-SESSION-MARKER") != NULL);
    CHECK(strstr(old, "OLDER-SESSION-MARKER") == NULL);
    CHECK(strstr(cur, "PREVIOUS-SESSION-MARKER") == NULL);
    // Many sessions later there are still only the two files.
    for (int i = 0; i < 5; i++) {
        sdk_load(s);
        sdk_unload(s);
    }
    CHECK_EQ_INT(count_logs(s->root), 2);
}

TEST(log_in_use_by_another_instance_goes_to_a_pid_log)
{
    // A game instance that is still exiting keeps braindance3d.log open (msvcrt shares read/write, not delete).
    Sdk *s = sdk_prepare(NULL);
    REQUIRE(tu_write_text(s->log, "STILL-WRITING\n"));
    HANDLE other = CreateFileW(s->log, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    REQUIRE(other != INVALID_HANDLE_VALUE);
    sdk_load(s);
    wchar_t pidlog[TU_PATH];
    _snwprintf(pidlog, TU_PATH, L"%ls\\braindance3d-%lu.log", s->root, GetCurrentProcessId());
    CHECK_MSG(tu_exists(pidlog), "no %ls", pidlog);
    CloseHandle(other);
    char *t = tu_read_file(s->log, NULL);
    REQUIRE(t);
    tt_defer(sdk_free_deferred, t);
    CHECK_EQ_STR(t, "STILL-WRITING\n"); // the other instance's log is left alone
    CHECK(!tu_exists(s->oldlog));
    sdk_unload(s);
    CHECK(tu_file_size(pidlog) > 0);
    CHECK_MSG(DeleteFileW(pidlog), "pid log still locked after unload (%lu)", GetLastError());
}

TEST(log_can_be_read_while_the_game_runs_and_is_released_on_unload)
{
    Sdk *s = sdk_loaded(NULL);
    HANDLE h = CreateFileW(s->log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK_MSG(h != INVALID_HANDLE_VALUE, "cannot open the log for reading while loaded (%lu)", GetLastError());
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    // Content is flushed as it is written, so a crash keeps it.
    CHECK(tu_file_size(s->log) > 0);
    sdk_unload(s);
    CHECK_MSG(DeleteFileW(s->log), "log still locked after FreeLibrary (%lu)", GetLastError());
}

TEST(settings_are_read_from_the_install_folder_only)
{
    // Level=0 next to the DLL or in GameLauncherData has no effect: only <InstallPath>\Braindance3D-Settings.ini counts.
    Sdk *s = sdk_prepare(NULL);
    wchar_t p[TU_PATH];
    tu_join(p, TU_PATH, s->core, SETTINGS_FILE);
    REQUIRE(tu_write_text(p, "[Log]\nLevel=0\n"));
    tu_join(p, TU_PATH, s->root, L"GameLauncherData\\" SETTINGS_FILE);
    REQUIRE(tu_write_text(p, "[Log]\nLevel=0\n"));
    sdk_load(s);
    CHECK(tu_exists(s->log));
    sdk_unload(s);
    DeleteFileW(s->log);
    tu_join(p, TU_PATH, s->root, SETTINGS_FILE);
    REQUIRE(tu_write_text(p, "[Log]\nLevel=0\n"));
    sdk_load(s);
    CHECK(!tu_exists(s->log));
}

TEST(non_ascii_install_folder)
{
    Sdk *s = sdk_prepare_in(tu_tempdir(L"u"), TU_NONASCII L" [x] (y)", NULL);
    sdk_load(s);
    Obj *dev = sdk_device(s);
    CHECK_EQ_INT(SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev), 1);
    device_release(dev);
    CHECK_MSG(tu_exists(s->log), "no log in %ls", s->root);
    sdk_unload(s);
    // The install folder is logged in UTF-8.
    char *t = tu_read_file(s->log, NULL);
    REQUIRE(t);
    tt_defer(sdk_free_deferred, t);
    char want[512];
    tu_utf8(TU_NONASCII, want, sizeof(want));
    CHECK_MSG(strstr(t, want) != NULL, "UTF-8 folder name not found in the log");
}

TEST(non_ascii_settings_file_level_0)
{
    Sdk *s = sdk_prepare_in(tu_tempdir(L"u0"), TU_NONASCII, "[Log]\nLevel=0\n");
    sdk_load(s);
    CHECK(!tu_exists(s->log));
}

// Makes every log name in the install folder impossible to create: they are non-empty folders.
static void block_install_folder_log(Sdk *s)
{
    wchar_t names[3][TU_PATH], p[TU_PATH];
    wcscpy(names[0], s->log);
    wcscpy(names[1], s->oldlog);
    _snwprintf(names[2], TU_PATH, L"%ls\\braindance3d-%lu.log", s->root, GetCurrentProcessId());
    for (int i = 0; i < 3; i++) {
        REQUIRE(tu_mkdirs(names[i]));
        tu_join(p, TU_PATH, names[i], L"keep.txt");
        tu_write_text(p, "x");
    }
}

TEST(log_falls_back_to_localappdata_when_install_folder_log_cannot_be_written)
{
    wchar_t *lad = tu_tempdir(L"lad");
    tu_setenv(L"LOCALAPPDATA", lad);
    Sdk *s = sdk_prepare(NULL);
    block_install_folder_log(s);
    sdk_load(s);
    wchar_t p[TU_PATH];
    tu_join(p, TU_PATH, lad, L"Braindance3D\\braindance3d.log");
    CHECK_MSG(tu_exists(p), "no fallback log at %ls", p);
    Obj *dev = sdk_device(s);
    CHECK_EQ_INT(SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev), 1);
    device_release(dev);
    sdk_unload(s);
    CHECK(tu_file_size(p) > 0);
    // The fallback log rotates too.
    sdk_load(s);
    sdk_unload(s);
    tu_join(p, TU_PATH, lad, L"Braindance3D\\braindance3d.old.log");
    CHECK(tu_exists(p));
}

static void restore_acl(void *dir)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;OICI;FA;;;WD)", SDDL_REVISION_1, &sd, NULL)) {
        SetFileSecurityW((const wchar_t *)dir, DACL_SECURITY_INFORMATION, sd);
        LocalFree(sd);
    }
}

TEST(log_falls_back_to_localappdata_when_install_folder_is_read_only)
{
    // A real "Program Files" situation: the folder denies creating files.
    wchar_t *lad = tu_tempdir(L"lad");
    tu_setenv(L"LOCALAPPDATA", lad);
    Sdk *s = sdk_prepare(NULL);
    PSECURITY_DESCRIPTOR sd = NULL;
    // Everyone: deny "create files" on this folder only; allow everything else.
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x2;;;WD)(A;OICI;FA;;;WD)", SDDL_REVISION_1,
                                                                 &sd, NULL));
    BOOL ok = SetFileSecurityW(s->root, DACL_SECURITY_INFORMATION, sd);
    LocalFree(sd);
    if (!ok) SKIP("cannot set an ACL here (%lu)", GetLastError());
    tt_defer(restore_acl, s->root);
    wchar_t probe[TU_PATH];
    tu_join(probe, TU_PATH, s->root, L"probe.txt");
    if (tu_write_text(probe, "x")) {
        DeleteFileW(probe);
        SKIP("ACLs are not enforced here (Wine, or a backup-privileged account)");
    }
    sdk_load(s);
    wchar_t p[TU_PATH];
    tu_join(p, TU_PATH, lad, L"Braindance3D\\braindance3d.log");
    CHECK_MSG(tu_exists(p), "no fallback log at %ls", p);
    CHECK(!tu_exists(s->log));
}

TEST(no_log_anywhere_still_works)
{
    tu_setenv(L"LOCALAPPDATA", NULL);
    Sdk *s = sdk_prepare(NULL);
    block_install_folder_log(s);
    sdk_load(s);
    Obj *f = sdk_factory(s);
    Obj *dev = sdk_device(s);
    float e[3];
    SLOT(dev, DEV_GET_EYE_POSITION, PFN_GetFloat3I)(dev, e, 0);
    CHECK_NEAR(e[0], -32.0, 1e-6);
    SharedPtr sp = { 0 };
    SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, dev, NULL, NULL, NULL, 0, 0, NULL);
    sp_release(&sp);
    device_release(dev);
}

TEST(localappdata_too_long_is_ignored)
{
    wchar_t lad[400];
    wcscpy(lad, L"C:\\");
    for (int i = 0; i < 30; i++) wcscat(lad, L"verylongname");
    tu_setenv(L"LOCALAPPDATA", lad);
    Sdk *s = sdk_prepare(NULL);
    block_install_folder_log(s);
    sdk_load(s);
    Obj *dev = sdk_device(s);
    CHECK_EQ_INT(SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev), 1);
    device_release(dev);
}

TEST(load_unload_cycles)
{
    Sdk *s = sdk_prepare(NULL);
    for (int i = 0; i < 25; i++) {
        sdk_load(s);
        Obj *dev = sdk_device(s);
        CHECK_EQ_INT(SLOT(dev, DEV_IS_AVAILABLE, PFN_IsAvailable)(dev), 1);
        device_release(dev);
        sdk_unload(s);
        REQUIRE_MSG(GetModuleHandleW(s->dll) == NULL, "the DLL did not unload (cycle %d)", i);
    }
}

TEST(two_copies_loaded_side_by_side)
{
    // E.g. two installs of the mod: each copy uses its own folder.
    wchar_t *tmp = tu_tempdir(L"two");
    Sdk *a = sdk_prepare_in(tmp, L"A", NULL), *b = sdk_prepare_in(tmp, L"B", "[Log]\nLevel=0\n");
    sdk_load(a);
    sdk_load(b);
    CHECK(a->h != b->h);
    CHECK(tu_exists(a->log));
    CHECK(!tu_exists(b->log));
    Obj *da = sdk_device(a), *db = sdk_device(b);
    CHECK_EQ_INT(SLOT(da, DEV_IS_AVAILABLE, PFN_IsAvailable)(da), 1);
    CHECK_EQ_INT(SLOT(db, DEV_IS_AVAILABLE, PFN_IsAvailable)(db), 1);
    device_release(da);
    device_release(db);
}

// ===================================================================================== [Display] PanelSize

TEST(panel_size_setting_overrides_slot_18)
{
    // log: what the startup line must contain ("not understood" when a value is rejected)
    struct { const char *ini; float w, h; const char *log; } cases[] = {
        { "[Display]\nPanelSize=59.77 x 33.62\n", 59.77f, 33.62f, "panel size 59.77 x 33.62 cm" },
        { "[Display]\r\nPanelSize = 59.77X33.62 ; 27 inch\r\n", 59.77f, 33.62f, "panel size 59.77 x 33.62 cm" },
        { "[Display]\nPanelSize=120\tx\t68 # big TV\n", 120.0f, 68.0f, "panel size 120.00 x 68.00 cm" },
        { "[Display]\nPanelSize=1x100000\n", 1.0f, 100000.0f, "panel size 1.00 x 100000.00 cm" },
        { "[Display]\nPanelSize=default\n", 10000.0f, 10000.0f, "panel size default" },
        { "[Display]\nPanelSize=\n", 10000.0f, 10000.0f, "panel size default" },
        { "[Display]\nPanelSize=Default ; same as no value\n", 10000.0f, 10000.0f, "panel size default" },
        { "[Display]\n", 10000.0f, 10000.0f, "panel size default" },
        { "[Display]\nPanelSize=59.77\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=120,68\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=59,77 x 33,62\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=60 ; 34\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=60;34\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=0 x 33\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=-60 x 34\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=60 x 100000.1\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=nan x 34\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=inf x 34\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=60 x 34 cm\n", 10000.0f, 10000.0f, "not understood" },
        { "[Display]\nPanelSize=abc\n", 10000.0f, 10000.0f, "not understood" },
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
        Sdk *s = sdk_prepare(cases[k].ini);
        sdk_load(s);
        Obj *dev = sdk_device(s);
        float v[2] = { 0, 0 };
        SLOT(dev, DEV_GET_FLOAT2_18, PFN_GetFloat2)(dev, v);
        CHECK_MSG(fabs(v[0] - cases[k].w) < 1e-3 && fabs(v[1] - cases[k].h) < 1e-3,
                  "settings \"%s\" -> %.2f x %.2f, want %.2f x %.2f", cases[k].ini, v[0], v[1], cases[k].w, cases[k].h);
        device_release(dev);
        sdk_unload(s);
        char *t = tu_read_file(s->log, NULL);
        REQUIRE(t != NULL);
        // Freed here, not with tt_defer: this many cases would fill the test's defer slots.
        CHECK_MSG(strstr(t, cases[k].log) != NULL, "settings \"%s\": the log does not say \"%s\"", cases[k].ini, cases[k].log);
        free(t);
    }
}

// ===================================================================================== [Display] Monitor

TEST(monitor_primary_setting_uses_the_primary_monitor)
{
    RECT mons[16] = {{0}};
    EnumDisplayMonitors(NULL, NULL, collect_monitors, (LPARAM)mons);
    int n = (int)mons[0].left;
    // Put the "game window" on a non-primary monitor when there is one.
    RECT primary = monitor_rect_of(NULL), target = mons[1];
    for (int i = 0; i < n; i++)
        if (!EqualRect(&mons[1 + i], &primary)) target = mons[1 + i];
    const char *inis[] = { "[Display]\nMonitor=primary\n", "[Display]\r\nMonitor = PRIMARY\r\n" };
    for (int k = 0; k < 2; k++) {
        Sdk *s = sdk_prepare(inis[k]);
        HWND w = tu_window(L"game", target.left + 10, target.top + 10, 400, 300);
        sdk_load(s);
        Obj *dev = sdk_device(s);
        int64_t rect[4];
        SLOT(dev, DEV_GET_MONITOR_RECT, PFN_GetRect)(dev, rect);
        CHECK(rect_matches(rect, primary));
        float res[2];
        SLOT(dev, DEV_GET_RESOLUTION, PFN_GetFloat2)(dev, res);
        CHECK_EQ_INT((int)res[0], primary.right - primary.left);
        device_release(dev);
        sdk_unload(s);
        DestroyWindow(w);
    }
    if (n < 2) tt_note("only one monitor: primary and the window's monitor are the same here");
}

TEST(monitor_setting_auto_or_unknown_follows_the_window)
{
    const char *inis[] = { "[Display]\nMonitor=auto\n", "[Display]\nMonitor=second\n", "[Display]\nMonitor=\n" };
    for (int k = 0; k < 3; k++) {
        Sdk *s = sdk_prepare(inis[k]);
        HWND w = tu_window(L"game", 50, 50, 400, 300);
        sdk_load(s);
        Obj *dev = sdk_device(s);
        int64_t rect[4];
        CHECK_MSG(wait_for_monitor(dev, monitor_rect_of(w), rect, 3000), "settings \"%s\"", inis[k]);
        device_release(dev);
        sdk_unload(s);
        DestroyWindow(w);
    }
}

TEST(monitor_rect_is_current_without_waiting)
{
    RECT mons[16] = {{0}};
    EnumDisplayMonitors(NULL, NULL, collect_monitors, (LPARAM)mons);
    if (mons[0].left < 2) SKIP("needs two monitors");
    Sdk *s = sdk_loaded(NULL);
    Obj *dev = sdk_device(s);
    HWND w = tu_window(L"game", mons[1].left + 10, mons[1].top + 10, 400, 300);
    for (int k = 0; k < 4; k++) {
        RECT m = mons[1 + (k & 1)];
        SetWindowPos(w, NULL, m.left + 20, m.top + 20, 400, 300, SWP_NOZORDER | SWP_NOACTIVATE);
        int64_t rect[4];
        SLOT(dev, DEV_GET_MONITOR_RECT, PFN_GetRect)(dev, rect);
        CHECK(rect_matches(rect, monitor_rect_of(w)));
    }
    device_release(dev);
}

// ===================================================================================== host check

TEST(factory_is_refused_outside_cyberpunk2077_exe)
{
    // The registry key is shared by every Odyssey 3D game: other programs get no factory.
    Sdk *s = sdk_prepare(NULL);
    const wchar_t *values[] = { NULL, L"0", L"true", L"11", L"yes" };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        tu_setenv(L"BRAINDANCE3D_ANY_EXE", values[i]);
        sdk_load(s);
        CHECK_MSG(s->create() == NULL, "factory offered with BRAINDANCE3D_ANY_EXE=%ls", values[i] ? values[i] : L"(unset)");
        s->destroy(NULL);
        CHECK(s->version != NULL && strcmp(s->version(), BD3D_VERSION_STR) == 0);
        sdk_unload(s);
    }
    tu_setenv(L"BRAINDANCE3D_ANY_EXE", L"1");
    sdk_load(s);
    CHECK(s->create() != NULL);
}

// Runs a copy of this test program named `exe_name` that loads the DLL and reports whether it got a factory.
static DWORD run_host_check(const wchar_t *dir, const wchar_t *exe_name, const wchar_t *dll)
{
    wchar_t self[TU_PATH], exe[TU_PATH], cmd[TU_PATH * 3];
    GetModuleFileNameW(NULL, self, TU_PATH);
    tu_join(exe, TU_PATH, dir, exe_name);
    REQUIRE_MSG(CopyFileW(self, exe, FALSE), "copy to %ls failed (%lu)", exe, GetLastError());
    _snwprintf(cmd, TU_PATH * 3, L"\"%ls\" --host-check \"%ls\"", exe, dll);
    cmd[TU_PATH * 3 - 1] = 0;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    REQUIRE_MSG(CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi), "CreateProcess failed (%lu)",
                GetLastError());
    REQUIRE(WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

TEST(factory_is_offered_in_cyberpunk2077_exe_case_insensitively)
{
    Sdk *s = sdk_prepare("[Log]\nLevel=0\n");
    wchar_t *bin = tu_tempdir(L"host");
    tu_setenv(L"BRAINDANCE3D_ANY_EXE", NULL); // the child inherits this environment
    CHECK_EQ_INT(run_host_check(bin, L"Cyberpunk2077.exe", s->dll), 0);
    wchar_t *bin2 = tu_tempdir(L"host2");
    CHECK_EQ_INT(run_host_check(bin2, L"CYBERPUNK2077.EXE", s->dll), 0);
    wchar_t *bin3 = tu_tempdir(L"host3");
    CHECK_EQ_INT(run_host_check(bin3, L"Cyberpunk2077-Launcher.exe", s->dll), 3);
    wchar_t *bin4 = tu_tempdir(L"host4");
    CHECK_EQ_INT(run_host_check(bin4, L"NotCyberpunk2077.exe", s->dll), 3);
}

static int host_check_main(const char *dll)
{
    wchar_t w[TU_PATH];
    MultiByteToWideChar(CP_ACP, 0, dll, -1, w, TU_PATH);
    HMODULE h = LoadLibraryExW(w, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) return 2;
    PFN_create create = (PFN_create)(void *)GetProcAddress(h, "create_odyssey3d_factory_instance");
    Obj *f = create ? create() : NULL;
    int ok = f && f->vtbl;
    FreeLibrary(h);
    return ok ? 0 : 3;
}

// ===================================================================================== window messages

typedef struct {
    Obj *f;
    HWND hwnd;
} WeaverThreadArgs;

static DWORD WINAPI create_weaver_thread(LPVOID p)
{
    WeaverThreadArgs *a = p;
    SharedPtr sp = { 0 };
    SLOT(a->f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(a->f, &sp, NULL, NULL, NULL, NULL, 0, 0, a->hwnd);
    sp_release(&sp);
    return 0;
}

typedef struct {
    Obj *dev;
} MonitorThreadArgs;

static DWORD WINAPI monitor_thread(LPVOID p)
{
    Obj *dev = ((MonitorThreadArgs *)p)->dev;
    for (int i = 0; i < 20; i++) {
        int64_t r[4];
        float res[2];
        SLOT(dev, DEV_GET_MONITOR_RECT, PFN_GetRect)(dev, r);
        SLOT(dev, DEV_GET_RESOLUTION, PFN_GetFloat2)(dev, res);
        Sleep(30);
    }
    return 0;
}

// Waits for th without pumping this (the window's) thread's messages, like a game main thread blocked on
// its render thread; returns 1 if th finished. Unblocks th either way.
static int finishes_while_window_thread_is_busy(HANDLE th, DWORD ms)
{
    DWORD w = WaitForSingleObject(th, ms);
    while (MsgWaitForMultipleObjects(1, &th, FALSE, 10000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) tu_pump();
    CloseHandle(th);
    return w == WAIT_OBJECT_0;
}

TEST(create_weaver_does_not_block_when_the_window_thread_is_busy)
{
    Sdk *s = sdk_loaded("[Log]\nLevel=2\n");
    Obj *f = sdk_factory(s);
    HWND hwnd = tu_window(L"Cyberpunk 2077", 0, 0, 320, 200);
    WeaverThreadArgs a = { f, hwnd };
    HANDLE th = CreateThread(NULL, 0, create_weaver_thread, &a, 0, NULL);
    REQUIRE(th != NULL);
    CHECK_MSG(finishes_while_window_thread_is_busy(th, 3000),
              "CreateWeaver on another thread waited for the window's thread (a window message was sent)");
}

TEST(monitor_queries_do_not_block_when_the_window_thread_is_busy)
{
    Sdk *s = sdk_loaded("[Log]\nLevel=2\n");
    Obj *dev = sdk_device(s);
    tu_window(L"Cyberpunk 2077", 0, 0, 320, 200);
    MonitorThreadArgs a = { dev };
    HANDLE th = CreateThread(NULL, 0, monitor_thread, &a, 0, NULL);
    REQUIRE(th != NULL);
    CHECK_MSG(finishes_while_window_thread_is_busy(th, 10000),
              "GetMonitorRect/GetResolution on another thread waited for the window's thread");
    device_release(dev);
}

// ===================================================================================== shared_ptr control block

TEST(weaver_control_block_vtable_has_msvc_layout)
{
    // MSVC _Ref_count_base virtuals: _Destroy, _Delete_this, scalar deleting destructor, _Get_deleter.
    Sdk *s = sdk_loaded(NULL);
    Obj *f = sdk_factory(s);
    SharedPtr sp = { 0 };
    SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL);
    REQUIRE(sp.rc != NULL);
    for (int i = 0; i < 4; i++) CHECK_MSG(tu_in_code(s->h, sp.rc->vtbl[i]), "control block vtable slot %d", i);
    if (!tu_in_code(s->h, sp.rc->vtbl[3]) || !tu_in_code(s->h, sp.rc->vtbl[2])) {
        sp_release(&sp);
        return;
    }
    static const char fake_type_info[64];
    CHECK(((PFN_RcGetDeleter)sp.rc->vtbl[3])(sp.rc, fake_type_info) == NULL);
    // Destructor with flags 0 returns this and frees nothing: the normal release still works afterwards.
    typedef RefCount *(*PFN_RcDtor)(RefCount *, unsigned);
    CHECK_EQ_PTR(((PFN_RcDtor)sp.rc->vtbl[2])(sp.rc, 0), sp.rc);
    CHECK_EQ_INT(sp.rc->uses, 1);
    sp_release(&sp);
}

TEST(weaver_control_block_destructor_with_flags_1_frees_it)
{
    Sdk *s = sdk_loaded("[Log]\nLevel=0\n");
    Obj *f = sdk_factory(s);
    typedef RefCount *(*PFN_RcDtor)(RefCount *, unsigned);
    {
        SharedPtr sp = { 0 };
        SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL);
        REQUIRE(sp.rc != NULL);
        int ok = tu_in_code(s->h, sp.rc->vtbl[2]);
        sp_release(&sp);
        REQUIRE_MSG(ok, "control block vtable has no destructor slot 2");
    }
    for (int i = 0; i < 200; i++) { // warm up
        SharedPtr sp = { 0 };
        SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL);
        ((PFN_RcVoid)sp.rc->vtbl[0])(sp.rc);
        ((PFN_RcDtor)sp.rc->vtbl[2])(sp.rc, 1);
    }
    long long before = tu_heap_busy_bytes();
    if (before < 0) SKIP("HeapWalk is not available");
    for (int i = 0; i < 10000; i++) {
        SharedPtr sp = { 0 };
        SLOT(f, FAC_CREATE_WEAVER, PFN_CreateWeaver)(f, &sp, NULL, NULL, NULL, NULL, 0, 0, NULL);
        REQUIRE(sp.rc);
        ((PFN_RcVoid)sp.rc->vtbl[0])(sp.rc);
        CHECK_EQ_PTR(((PFN_RcDtor)sp.rc->vtbl[2])(sp.rc, 1), sp.rc);
    }
    CHECK(tu_heap_busy_bytes() - before < 64 * 1024);
}

// ===================================================================================== main

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--host-check")) return host_check_main(argv[2]);
    tt_set_args(argc, argv);
    // The DLL only offers its factory inside Cyberpunk2077.exe unless this is set.
    SetEnvironmentVariableW(L"BRAINDANCE3D_ANY_EXE", L"1");
    // Keep any fallback log the DLL might write out of the real %LOCALAPPDATA%.
    wchar_t tmp[TU_PATH], lad[TU_PATH];
    GetTempPathW(TU_PATH, tmp);
    _snwprintf(lad, TU_PATH, L"%lsbd3d-tests-%lu-localappdata", tmp, GetCurrentProcessId());
    lad[TU_PATH - 1] = 0;
    CreateDirectoryW(lad, NULL);
    SetEnvironmentVariableW(L"LOCALAPPDATA", lad);
    char path[TU_PATH * 3];
    tu_utf8(tu_dll_path(), path, sizeof(path));
    printf("DLL under test: %s%s\n", path, tu_running_under_wine() ? " (running under Wine)" : "");
    int rc = tt_main(argc, argv);
    tu_rmrf(lad);
    return rc;
}
