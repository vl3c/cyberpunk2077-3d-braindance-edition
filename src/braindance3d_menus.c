// 3D Braindance Edition: makes menus readable in half side-by-side 3D.
//
// The game draws menus as one flat 2D picture across the whole window. A headset in half-SBS mode
// shows the left half of that picture to the left eye and the right half to the right eye, so each
// eye sees a different, stretched half. While a menu is open this squeezes the whole frame into the
// left half and copies it into the right half, just before it is presented: both eyes then see the
// same picture, as one flat screen.
//
// How it reaches the frame (part of the RED4ext plugin, so it needs RED4ext):
//   - RED4ext resolves two game addresses by hash, the same ones Cyber Engine Tweaks uses:
//     GpuApi::Present and the GPU device data (swap chains, direct command queue).
//   - A pass-through hook on GpuApi::Present finds the game's swap chain on its first call and
//     patches that swap chain's Present and Present1 vtable slots. At that point the back buffer is
//     finished and in the PRESENT state.
//   - The squeeze is a copy of the back buffer and one full-screen draw, recorded on our own command
//     list and executed on the game's direct queue, so it runs after the game's work for the frame.
//     It sits in front of OptiScaler, Streamline and ReShade's own Present wrappers when those wrap
//     the swap chain the game holds, and after them otherwise.
//   - The shaders are compiled when the plugin loads, with Windows' own d3dcompiler_47.dll.
//   - Only the game's own swap chain is squeezed (other swap chains can share the patched vtable), at
//     most once per game frame.
// Any failure turns the squeeze off for the rest of the session and says why in the log. The game
// itself never stops because of it.
//
// When it squeezes ([Menus] Squeeze in Braindance3D-Settings.ini):
//   auto (default): on flat frames while 3D Mode is on. Once per game frame (GpuApi::Present) this reads
//                   the stand-in's call counters (braindance3d_device_calls):
//                   - eye positions asked: a stereo frame, so 3D Mode is on and nothing is squeezed;
//                   - no eye positions: a flat frame (menus, the map, loading screens), squeezed if 3D
//                     Mode is on;
//                   - one IsAvailable and no eye positions for STEREO_OFF_FRAMES frames in a row: 2D
//                     gameplay, so 3D Mode is off;
//                   - the game's 3D Mode handler (hooked in braindance3d_framegen.c) says which way 3D Mode
//                     was switched in Settings, or set at start-up or on a display change.
//                   At start-up 3D Mode's last saved setting (UserSettings.json) is the first guess.
//   on:             every frame (for testing)
//   off:            never; the squeeze hooks aren't installed (the frame generation hooks still are)
// Log: Braindance3D-hooks.log next to the settings file ([Log] Level 0 off, 1 normal, 2 verbose), shared with the
// frame generation hooks (braindance3d_framegen.c).
//
// License: MIT (see LICENSE).

#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#include "braindance3d_menus.h"

// ---------------------------------------------------------------- game addresses (RED4ext hashes)

#define HASH_GPUAPI_PRESENT 2468877568u // GpuApi::Present (CET: CRenderNode_Present_DoInternal)
#define HASH_DEVICE_DATA 1239944840u    // GpuApi::SDeviceData* (RED4ext.SDK g_DeviceData)
// Offsets in SDeviceData (RED4ext.SDK GpuApi/DeviceData.hpp, CET reverse/RenderContext.h).
#define DD_SWAPCHAINS 0xCEBF48u   // SSwapChainData entries, the first member is IDXGISwapChain4*
#define DD_SWAPCHAIN_STRIDE 0xB0u
#define DD_SWAPCHAIN_COUNT 32
#define DD_DIRECT_QUEUE 0x14104D0u // ID3D12CommandQueue*

#define VT_PRESENT 8   // IDXGISwapChain::Present
#define VT_PRESENT1 22 // IDXGISwapChain1::Present1

#define RING 4 // command allocators in flight

typedef void *(*GpuPresentFn)(int32_t *deviceIndex, uint8_t sync, UINT interval);
typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain *sc, UINT interval, UINT flags);
typedef HRESULT(STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain1 *sc, UINT interval, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS *params);
typedef uintptr_t (*ResolveFn)(uint32_t hash);
typedef LONG64 (*DeviceCallsFn)(int slot);
typedef HRESULT(WINAPI *D3DCompileFn)(const void *src, SIZE_T len, const char *name, const D3D_SHADER_MACRO *defines,
                                      ID3DInclude *include, const char *entry, const char *target, UINT flags1,
                                      UINT flags2, ID3DBlob **code, ID3DBlob **errors);
typedef HRESULT(WINAPI *SerializeRootSigFn)(const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION v,
                                            ID3DBlob **blob, ID3DBlob **errors);

enum { MODE_OFF, MODE_AUTO, MODE_ON };
enum { STEREO_UNKNOWN = -1, STEREO_OFF, STEREO_ON }; // whether the game's 3D Mode is on

// Stand-in device slots read once per game frame (see braindance3d_device_calls in braindance3d_sdk.c).
#define SLOT_EYE_POSITION 1
#define SLOT_IS_AVAILABLE 10
#define STEREO_OFF_FRAMES 30 // 2D gameplay frames in a row before 3D Mode counts as off

static struct {
    int mode, level;
    FILE *log;
    CRITICAL_SECTION logLock;
    uintptr_t deviceData; // address of the SDeviceData* variable
    GpuPresentFn gpuPresentOrig;
    volatile LONG patched, failed;
    void **vtbl;
    PresentFn presentOrig;
    Present1Fn present1Orig;
    DeviceCallsFn deviceCalls;
    LONG64 lastEye, lastAvailable;
    volatile LONG flatFrame; // the game's last frame asked for no eye positions
    volatile LONG stereo;    // STEREO_*: whether 3D Mode is on, as far as the frames tell
    int flatStreak;          // 2D gameplay frames in a row
    int scIndex;             // the game's swap chain: entry in the device data (device index - 1)
    volatile LONG busy;      // a squeeze is being recorded (guards against re-entry from wrappers)
    LONG64 lastSqueezeFrame; // game frame of the last squeeze (one per frame at most)
    LONG64 gameFrames;
    int lastSqueeze;  // for logging changes
    LONG64 frames, squeezed;
    DWORD lastSummary;
    // D3D12 state, created on the first squeeze
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12RootSignature *rootSig;
    ID3D12PipelineState *pso;
    DXGI_FORMAT psoFormat;
    ID3D12DescriptorHeap *rtvHeap, *srvHeap;
    ID3D12Resource *copy;
    UINT64 copyW;
    UINT copyH;
    DXGI_FORMAT copyFormat;
    ID3D12CommandAllocator *alloc[RING];
    UINT64 allocFence[RING];
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    UINT64 fenceValue;
    HANDLE fenceEvent;
    int ring;
    ID3DBlob *vs, *ps;
} g;

// ---------------------------------------------------------------- logging

static void vmlog(const char *fmt, va_list ap)
{
    if (!g.log) return;
    EnterCriticalSection(&g.logLock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g.log, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] ", st.wYear, st.wMonth, st.wDay, st.wHour,
            st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    vfprintf(g.log, fmt, ap);
    fputc('\n', g.log);
    fflush(g.log);
    LeaveCriticalSection(&g.logLock);
}

static void mlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmlog(fmt, ap);
    va_end(ap);
}

void plugin_log(int level, const char *fmt, ...)
{
    if (g.level < level) return;
    va_list ap;
    va_start(ap, fmt);
    vmlog(fmt, ap);
    va_end(ap);
}

#define VLOG(...) do { if (g.level >= 2) mlog(__VA_ARGS__); } while (0)

// Turns the squeeze off for this session. Returns 0 so callers can `return fail(...)`.
static int fail(const char *what, HRESULT hr)
{
    if (InterlockedExchange(&g.failed, 1) == 0)
        mlog("ERROR: %s (hr=0x%08lX); menu squeeze is off for the rest of this session", what, (unsigned long)hr);
    return 0;
}

static FILE *open_hooks_log_in(const wchar_t *dir)
{
    wchar_t path[MAX_PATH], old[MAX_PATH];
    if (_snwprintf(path, MAX_PATH, L"%ls\\Braindance3D-hooks.log", dir) < 0) return NULL;
    if (_snwprintf(old, MAX_PATH, L"%ls\\Braindance3D-hooks.old.log", dir) < 0) return NULL;
    path[MAX_PATH - 1] = old[MAX_PATH - 1] = 0;
    MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
    return _wfopen(path, L"w");
}

// Next to the settings file, or %LOCALAPPDATA%\Braindance3D if the game folder is read-only (like the stand-in's log).
static void open_hooks_log(const wchar_t *dir)
{
    g.log = open_hooks_log_in(dir);
    if (g.log) return;
    wchar_t local[MAX_PATH], fallback[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH || _snwprintf(fallback, MAX_PATH, L"%ls\\Braindance3D", local) < 0) return;
    fallback[MAX_PATH - 1] = 0;
    CreateDirectoryW(fallback, NULL);
    g.log = open_hooks_log_in(fallback);
}

// ---------------------------------------------------------------- settings

static void read_menu_settings(const wchar_t *dir)
{
    wchar_t ini[MAX_PATH], v[64];
    g.mode = MODE_AUTO;
    g.level = 1;
    if (_snwprintf(ini, MAX_PATH, L"%ls\\Braindance3D-Settings.ini", dir) < 0) return;
    ini[MAX_PATH - 1] = 0;
    GetPrivateProfileStringW(L"Log", L"Level", L"1", v, 64, ini);
    // Same rule as the stand-in's parse_level: no number is 1, 2 or more is 2.
    const wchar_t *t = v;
    while (*t == L' ' || *t == L'\t') t++;
    long n = *t >= L'0' && *t <= L'9' ? wcstol(t, NULL, 10) : 1;
    g.level = n <= 0 ? 0 : n >= 2 ? 2 : 1;
    GetPrivateProfileStringW(L"Menus", L"Squeeze", L"auto", v, 64, ini);
    if (_wcsnicmp(v, L"off", 3) == 0 && (!v[3] || wcschr(L" \t;#", v[3]))) g.mode = MODE_OFF;
    else if (_wcsnicmp(v, L"on", 2) == 0 && (!v[2] || wcschr(L" \t;#", v[2]))) g.mode = MODE_ON;
}

// ---------------------------------------------------------------- memory checks

// Whether [p, p + size) is committed, readable memory.
static int readable(const void *p, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (const char *)p + size <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

// Whether p looks like a COM object: readable, with a vtable whose first entries point into a loaded module.
static int looks_like_com(void *p)
{
    if (!readable(p, sizeof(void *))) return 0;
    void **vt = *(void ***)p;
    if (!readable(vt, 3 * sizeof(void *))) return 0;
    HMODULE m;
    for (int i = 0; i < 3; i++)
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)vt[i], &m))
            return 0;
    return 1;
}

static const char *module_of(const void *addr, char *buf, int size)
{
    HMODULE m;
    wchar_t w[MAX_PATH];
    buf[0] = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &m) && GetModuleFileNameW(m, w, MAX_PATH)) {
        const wchar_t *base = wcsrchr(w, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : w, -1, buf, size, NULL, NULL);
    }
    if (!buf[0]) snprintf(buf, size, "?");
    return buf;
}

// ---------------------------------------------------------------- the squeeze pass

static const char g_hlsl[] =
    "Texture2D frame : register(t0);\n"
    "SamplerState linearClamp : register(s0);\n"
    "void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)\n"
    "{\n"
    "    uv = float2((id << 1) & 2, id & 2);\n"
    "    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
    "{\n"
    "    // Each half of the screen shows the whole frame at half the width.\n"
    "    float x = uv.x < 0.5 ? uv.x * 2.0 : uv.x * 2.0 - 1.0;\n"
    "    return frame.Sample(linearClamp, float2(x, uv.y));\n"
    "}\n";

static int compile_shaders(void)
{
    HMODULE dc = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    D3DCompileFn compile = dc ? (D3DCompileFn)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) return fail("d3dcompiler_47.dll (System32) or its D3DCompile export not found", HRESULT_FROM_WIN32(GetLastError()));
    ID3DBlob *err = NULL;
    HRESULT hr = compile(g_hlsl, sizeof(g_hlsl) - 1, "braindance3d_menus", NULL, NULL, "vs", "vs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g.vs, &err);
    if (SUCCEEDED(hr))
        hr = compile(g_hlsl, sizeof(g_hlsl) - 1, "braindance3d_menus", NULL, NULL, "ps", "ps_5_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &g.ps, &err);
    if (FAILED(hr)) {
        if (err) mlog("shader compiler: %.*s", (int)ID3D10Blob_GetBufferSize(err), (const char *)ID3D10Blob_GetBufferPointer(err));
        if (err) ID3D10Blob_Release(err);
        return fail("compiling the squeeze shaders", hr);
    }
    if (err) ID3D10Blob_Release(err);
    mlog("squeeze shaders compiled (vs %u bytes, ps %u bytes)", (unsigned)ID3D10Blob_GetBufferSize(g.vs),
         (unsigned)ID3D10Blob_GetBufferSize(g.ps));
    return 1;
}

static int create_root_signature(void)
{
    HMODULE d12 = GetModuleHandleW(L"d3d12.dll");
    SerializeRootSigFn serialize = d12 ? (SerializeRootSigFn)(void *)GetProcAddress(d12, "D3D12SerializeRootSignature") : NULL;
    if (!serialize) return fail("d3d12.dll D3D12SerializeRootSignature not found", E_FAIL);
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER param = { 0 };
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = { 0 };
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc = { 1, &param, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) return fail("serializing the root signature", hr);
    hr = ID3D12Device_CreateRootSignature(g.device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                          &IID_ID3D12RootSignature, (void **)&g.rootSig);
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) return fail("CreateRootSignature", hr);
    return 1;
}

// Waits until the GPU has passed fence value v (an auto-reset event can be left signalled by an earlier
// wait that timed out, so the fence itself is checked again after every wake-up). Returns 0 on timeout.
static int wait_fence(UINT64 v, DWORD ms)
{
    DWORD start = GetTickCount();
    while (ID3D12Fence_GetCompletedValue(g.fence) < v) {
        DWORD spent = GetTickCount() - start;
        if (spent >= ms) return 0;
        if (FAILED(ID3D12Fence_SetEventOnCompletion(g.fence, v, g.fenceEvent))) return 0;
        WaitForSingleObject(g.fenceEvent, ms - spent);
    }
    return 1;
}

// Before a resource that earlier squeezes used is released: the GPU must be done with all of them.
static int idle_before_release(const char *what)
{
    if (wait_fence(g.fenceValue, 1000)) return 1;
    mlog("warning: GPU still busy after 1 s, can't replace the %s yet", what);
    return 0;
}

static int create_pso(DXGI_FORMAT format)
{
    if (g.pso && g.psoFormat == format) return 1;
    if (g.pso && !idle_before_release("pipeline")) return 0;
    if (g.pso) ID3D12PipelineState_Release(g.pso);
    g.pso = NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    memset(&d, 0, sizeof(d));
    d.pRootSignature = g.rootSig;
    d.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(g.vs);
    d.VS.BytecodeLength = ID3D10Blob_GetBufferSize(g.vs);
    d.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(g.ps);
    d.PS.BytecodeLength = ID3D10Blob_GetBufferSize(g.ps);
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = format;
    d.SampleDesc.Count = 1;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(g.device, &d, &IID_ID3D12PipelineState, (void **)&g.pso);
    if (FAILED(hr)) return fail("CreateGraphicsPipelineState", hr);
    g.psoFormat = format;
    mlog("squeeze pipeline created for DXGI format %d", (int)format);
    return 1;
}

static int create_copy_texture(const D3D12_RESOURCE_DESC *bb)
{
    if (g.copy && g.copyW == bb->Width && g.copyH == bb->Height && g.copyFormat == bb->Format) return 1;
    if (g.copy && !idle_before_release("copy texture")) return 0;
    if (g.copy) ID3D12Resource_Release(g.copy);
    g.copy = NULL;
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC d = *bb;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    d.MipLevels = 1;
    d.DepthOrArraySize = 1;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Alignment = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    HRESULT hr = ID3D12Device_CreateCommittedResource(g.device, &heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                                      NULL, &IID_ID3D12Resource, (void **)&g.copy);
    if (FAILED(hr)) return fail("creating the copy texture", hr);
    g.copyW = bb->Width;
    g.copyH = bb->Height;
    g.copyFormat = bb->Format;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    memset(&srv, 0, sizeof(srv));
    srv.Format = bb->Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(g.device, g.copy, &srv, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(g.srvHeap));
    mlog("copy texture %llux%u, DXGI format %d", (unsigned long long)bb->Width, bb->Height, (int)bb->Format);
    return 1;
}

// With DLSS frame generation loaded, Streamline hands the game proxy D3D12 objects (vtables in sl.interposer.dll), while
// the swap chain's back buffers belong to the native device. Returns the native object behind a Streamline proxy, with
// a reference added (slGetNativeInterface adds one), or p itself.
typedef int (*SlGetNativeInterfaceFn)(void *proxy, void **base);
static void *native_interface(void *p, const char *what)
{
    HMODULE sl = GetModuleHandleW(L"sl.interposer.dll"), owner;
    if (!sl || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)**(void ***)p, &owner) || owner != sl)
        return p;
    SlGetNativeInterfaceFn get = (SlGetNativeInterfaceFn)(void *)GetProcAddress(sl, "slGetNativeInterface");
    void *base = NULL;
    int r = get ? get(p, &base) : -1;
    if (r != 0 || !looks_like_com(base)) {
        mlog("warning: the %s is Streamline's proxy, and slGetNativeInterface gave no native one (result %d)", what, r);
        return p;
    }
    char mod[MAX_PATH];
    mlog("the %s is Streamline's proxy; using the native one %p (vtable in %s)", what, base,
         module_of(**(void ***)base, mod, sizeof(mod)));
    return base;
}

static int init_d3d(void)
{
    if (g.device) return 1;
    char mod[MAX_PATH];
    uintptr_t dd = readable((void *)g.deviceData, sizeof(void *)) ? *(uintptr_t *)g.deviceData : 0;
    void **slot = dd ? (void **)(dd + DD_DIRECT_QUEUE) : NULL;
    void *queue = readable(slot, sizeof(void *)) ? *slot : NULL;
    if (!looks_like_com(queue)) return fail("the game's direct command queue doesn't look valid (game version changed?)", E_POINTER);
    mlog("direct queue %p (vtable in %s)", queue, module_of(**(void ***)queue, mod, sizeof(mod)));
    void *game = queue;
    queue = native_interface(queue, "direct queue");
    ID3D12CommandQueue *q = NULL;
    HRESULT hr = IUnknown_QueryInterface((IUnknown *)queue, &IID_ID3D12CommandQueue, (void **)&q);
    if (queue != game) IUnknown_Release((IUnknown *)queue); // q holds its own reference
    if (FAILED(hr)) return fail("QueryInterface(ID3D12CommandQueue) on the game's direct queue", hr);
    D3D12_COMMAND_QUEUE_DESC qd = ID3D12CommandQueue_GetDesc(q);
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12CommandQueue_Release(q);
        return fail("the game's queue at the direct-queue offset isn't a direct queue", E_UNEXPECTED);
    }
    g.queue = q;
    hr = ID3D12CommandQueue_GetDevice(q, &IID_ID3D12Device, (void **)&g.device);
    if (FAILED(hr)) return fail("GetDevice on the direct queue", hr);
    if (!g.vs || !g.ps) return fail("the squeeze shaders weren't compiled", E_FAIL);
    if (!create_root_signature()) return 0;
    D3D12_DESCRIPTOR_HEAP_DESC rtv = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC srv = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    hr = ID3D12Device_CreateDescriptorHeap(g.device, &rtv, &IID_ID3D12DescriptorHeap, (void **)&g.rtvHeap);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateDescriptorHeap(g.device, &srv, &IID_ID3D12DescriptorHeap, (void **)&g.srvHeap);
    if (FAILED(hr)) return fail("CreateDescriptorHeap", hr);
    for (int i = 0; i < RING && SUCCEEDED(hr); i++)
        hr = ID3D12Device_CreateCommandAllocator(g.device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                                 (void **)&g.alloc[i]);
    if (FAILED(hr)) return fail("CreateCommandAllocator", hr);
    hr = ID3D12Device_CreateCommandList(g.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], NULL,
                                        &IID_ID3D12GraphicsCommandList, (void **)&g.list);
    if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Close(g.list);
    if (FAILED(hr)) return fail("CreateCommandList", hr);
    hr = ID3D12Device_CreateFence(g.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&g.fence);
    if (FAILED(hr)) return fail("CreateFence", hr);
    g.fenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g.fenceEvent) return fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    mlog("D3D12 ready: device %p, queue %p", (void *)g.device, (void *)g.queue);
    return 1;
}

static void barrier(D3D12_RESOURCE_BARRIER *b, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    memset(b, 0, sizeof(*b));
    b->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b->Transition.pResource = r;
    b->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b->Transition.StateBefore = from;
    b->Transition.StateAfter = to;
}

// Squeezes the current back buffer of sc. Called just before the swap chain presents it.
static void squeeze(IDXGISwapChain *sc)
{
    if (g.failed || !init_d3d()) return;
    IDXGISwapChain3 *sc3 = NULL;
    HRESULT hr = IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain3, (void **)&sc3);
    if (FAILED(hr)) { fail("QueryInterface(IDXGISwapChain3)", hr); return; }
    UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(sc3);
    ID3D12Resource *bb = NULL;
    hr = IDXGISwapChain3_GetBuffer(sc3, index, &IID_ID3D12Resource, (void **)&bb);
    IDXGISwapChain3_Release(sc3);
    if (FAILED(hr)) { fail("GetBuffer on the swap chain", hr); return; }
    ID3D12Device *bbDevice = NULL;
    hr = ID3D12Resource_GetDevice(bb, &IID_ID3D12Device, (void **)&bbDevice);
    if (bbDevice) ID3D12Device_Release(bbDevice);
    if (FAILED(hr) || bbDevice != g.device) {
        // Shouldn't happen for the game's swap chain (with Streamline the native queue and device are used, see
        // native_interface); never record another device's resource on the game's queue.
        ID3D12Resource_Release(bb);
        fail("the back buffer belongs to another D3D12 device", FAILED(hr) ? hr : E_UNEXPECTED);
        return;
    }
    D3D12_RESOURCE_DESC desc = ID3D12Resource_GetDesc(bb);
    if (!create_copy_texture(&desc) || !create_pso(desc.Format)) { ID3D12Resource_Release(bb); return; }

    int slot = g.ring;
    if (!wait_fence(g.allocFence[slot], 1000)) {
        mlog("warning: GPU still busy with an earlier squeeze after 1 s, skipping this frame");
        ID3D12Resource_Release(bb);
        return;
    }
    g.ring = (g.ring + 1) % RING;
    hr = ID3D12CommandAllocator_Reset(g.alloc[slot]);
    if (SUCCEEDED(hr)) hr = ID3D12GraphicsCommandList_Reset(g.list, g.alloc[slot], g.pso);
    if (FAILED(hr)) { ID3D12Resource_Release(bb); fail("resetting the command list", hr); return; }

    ID3D12GraphicsCommandList *cl = g.list;
    D3D12_RESOURCE_BARRIER b[2];
    barrier(&b[0], bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, b);
    ID3D12GraphicsCommandList_CopyResource(cl, g.copy, bb);
    barrier(&b[0], bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(&b[1], g.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 2, b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(g.rtvHeap);
    ID3D12Device_CreateRenderTargetView(g.device, bb, NULL, rtv);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT vp = { 0, 0, (float)desc.Width, (float)desc.Height, 0, 1 };
    D3D12_RECT scissor = { 0, 0, (LONG)desc.Width, (LONG)desc.Height };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &scissor);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, g.rootSig);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &g.srvHeap);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(g.srvHeap));
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);

    barrier(&b[0], bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    barrier(&b[1], g.copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 2, b);
    hr = ID3D12GraphicsCommandList_Close(cl);
    if (FAILED(hr)) { ID3D12Resource_Release(bb); fail("closing the command list", hr); return; }
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(g.queue, 1, lists);
    g.allocFence[slot] = ++g.fenceValue;
    ID3D12CommandQueue_Signal(g.queue, g.fence, g.fenceValue);
    ID3D12Resource_Release(bb);
    g.squeezed++;
    if (g.squeezed == 1) mlog("first squeeze: back buffer %u, %llux%u", index, (unsigned long long)desc.Width, desc.Height);
}

// ---------------------------------------------------------------- when to squeeze

static const char *stereo_name(LONG v) { return v == STEREO_ON ? "on" : v == STEREO_OFF ? "off" : "unknown"; }

static void set_stereo(LONG v, const char *why)
{
    if (InterlockedExchange(&g.stereo, v) != v)
        mlog("3D Mode now %s (%s, game frame %lld)", stereo_name(v), why, (long long)g.gameFrames);
}

// First guess at whether 3D Mode is on: the game saves its settings to UserSettings.json when it quits, so the
// file holds the setting this session started with. Only the "Odyssey3D_StereoEnabled" option object is read.
static void read_saved_stereo_setting(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) { mlog("3D Mode setting: no LOCALAPPDATA, starting as unknown"); return; }
    if (_snwprintf(path, MAX_PATH, L"%ls\\CD Projekt Red\\Cyberpunk 2077\\UserSettings.json", dir) < 0) return;
    path[MAX_PATH - 1] = 0;
    FILE *f = _wfopen(path, L"rb");
    if (!f) { mlog("3D Mode setting: %ls not found, starting as unknown", path); return; }
    static char buf[1 << 20]; // the file is about 100 KB
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;
    const char *opt = strstr(buf, "\"Odyssey3D_StereoEnabled\"");
    const char *end = opt ? strchr(opt, '}') : NULL;
    const char *val = opt ? strstr(opt, "\"value\"") : NULL;
    if (!opt || !end || !val || val > end) {
        mlog("3D Mode setting: no Odyssey3D_StereoEnabled value in UserSettings.json, starting as unknown");
        return;
    }
    val += 7;
    while (*val == ' ' || *val == '\t' || *val == ':' || *val == '\r' || *val == '\n') val++;
    if (strncmp(val, "true", 4) == 0) set_stereo(STEREO_ON, "saved setting in UserSettings.json");
    else if (strncmp(val, "false", 5) == 0) set_stereo(STEREO_OFF, "saved setting in UserSettings.json");
    else mlog("3D Mode setting: Odyssey3D_StereoEnabled value not understood, starting as unknown");
}

static void note_frame_calls(LONG64 dEye, LONG64 dAvail);

// Called once per game frame, from GpuApi::Present: what the game asked the stand-in since the last one.
static void note_game_frame(void)
{
    if (!g.deviceCalls) {
        // The stand-in is loaded by the game when it sets up 3D, after this plugin; look it up until it's there.
        if (g.gameFrames % 60 != 1) return;
        HMODULE sdk = GetModuleHandleW(L"Odyssey3DSDK.dll");
        g.deviceCalls = sdk ? (DeviceCallsFn)(void *)GetProcAddress(sdk, "braindance3d_device_calls") : NULL;
        if (!g.deviceCalls) return;
        // Pinned: this pointer is read every frame, so the stand-in must never unload under it.
        HMODULE pinned;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCWSTR)(void *)g.deviceCalls, &pinned);
        g.lastEye = g.deviceCalls(SLOT_EYE_POSITION);
        g.lastAvailable = g.deviceCalls(SLOT_IS_AVAILABLE);
        mlog("menu detection: the stand-in's call counters found (game frame %lld: %lld eye positions, %lld "
             "IsAvailable so far)", (long long)g.gameFrames, (long long)g.lastEye, (long long)g.lastAvailable);
        return;
    }
    LONG64 eye = g.deviceCalls(SLOT_EYE_POSITION), avail = g.deviceCalls(SLOT_IS_AVAILABLE);
    LONG64 dEye = eye - g.lastEye, dAvail = avail - g.lastAvailable;
    g.lastEye = eye;
    g.lastAvailable = avail;
    VLOG("game frame %lld: %lld eye positions, %lld IsAvailable -> %s", (long long)g.gameFrames, (long long)dEye,
         (long long)dAvail, dEye ? "stereo" : "flat");
    note_frame_calls(dEye, dAvail);
}

// What one game frame's stand-in calls say (split out of note_game_frame for the tests).
static void note_frame_calls(LONG64 dEye, LONG64 dAvail)
{
    LONG flat = dEye == 0;
    InterlockedExchange(&g.flatFrame, flat);
    if (!flat) {
        // Only a frame rendered in stereo asks for eye positions.
        g.flatStreak = 0;
        set_stereo(STEREO_ON, "a frame rendered in stereo");
        return;
    }
    if (dAvail == 1) {
        // 2D gameplay asks IsAvailable once per frame (stereo asks twice); a few such frames also turn up while
        // the settings menu re-checks the display, so it takes a run of them.
        if (++g.flatStreak == STEREO_OFF_FRAMES) set_stereo(STEREO_OFF, "2D gameplay frames");
    } else {
        g.flatStreak = 0;
    }
}

void menus_note_3d_mode(int on)
{
    // The handler runs on the main thread while frames go on. The 2D-gameplay run starts over (a plain store: the
    // render thread's next frame counts from there either way).
    g.flatStreak = 0;
    set_stereo(on ? STEREO_ON : STEREO_OFF, "the game's 3D Mode handler");
}

static int menu_open(void) { return g.deviceCalls && g.flatFrame && g.stereo == STEREO_ON; }

static int want_squeeze(void) { return g.mode == MODE_ON ? 1 : menu_open(); }

// Whether sc is the game's swap chain. The patched vtable can be shared by other swap chains (every DXGI swap
// chain, or every one Streamline wraps), and the game can recreate its own, so the entry is read every time.
static int is_game_swap_chain(IDXGISwapChain *sc)
{
    uintptr_t dd = readable((void *)g.deviceData, sizeof(void *)) ? *(uintptr_t *)g.deviceData : 0;
    void **entry = dd ? (void **)(dd + DD_SWAPCHAINS + (uintptr_t)g.scIndex * DD_SWAPCHAIN_STRIDE) : NULL;
    void *game = entry && readable(entry, sizeof(void *)) ? *entry : NULL;
    if (!game) return 0;
    if (game == (void *)sc) return 1;
    // Same object through another interface pointer: compare the IUnknown identities.
    IUnknown *a = NULL, *b = NULL;
    int same = 0;
    if (looks_like_com(game) && SUCCEEDED(IUnknown_QueryInterface((IUnknown *)game, &IID_IUnknown, (void **)&a)) &&
        SUCCEEDED(IDXGISwapChain_QueryInterface(sc, &IID_IUnknown, (void **)&b)))
        same = a == b;
    if (a) IUnknown_Release(a);
    if (b) IUnknown_Release(b);
    return same;
}

static void before_present(IDXGISwapChain *sc, const char *which)
{
    // One at a time: a wrapper whose Present calls Present1 on the same vtable, or two presenting threads, must
    // not record the shared command list twice.
    if (!is_game_swap_chain(sc)) return;
    if (InterlockedCompareExchange(&g.busy, 1, 0) != 0) return;
    g.frames++;
    int want = !g.failed && want_squeeze();
    if (want != g.lastSqueeze) {
        mlog("%s: squeeze %s (frame %lld)", which, want ? "ON" : "off", (long long)g.frames);
        g.lastSqueeze = want;
    }
    // At most once per game frame, so a frame presented twice isn't squeezed twice (four pictures).
    if (want && g.lastSqueezeFrame != g.gameFrames) {
        g.lastSqueezeFrame = g.gameFrames;
        squeeze(sc);
    }
    DWORD now = GetTickCount();
    if (g.level >= 2 && now - g.lastSummary >= 1000) {
        mlog("summary: %lld frames presented, %lld squeezed", (long long)g.frames, (long long)g.squeezed);
        g.lastSummary = now;
    }
    InterlockedExchange(&g.busy, 0);
}

// ---------------------------------------------------------------- hooks

static HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain *sc, UINT interval, UINT flags)
{
    if (!(flags & DXGI_PRESENT_TEST)) before_present(sc, "Present");
    return g.presentOrig(sc, interval, flags);
}

static HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1 *sc, UINT interval, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS *params)
{
    if (!(flags & DXGI_PRESENT_TEST)) before_present((IDXGISwapChain *)sc, "Present1");
    return g.present1Orig(sc, interval, flags, params);
}

static int patch_slot(void **slot, void *hook, void **orig)
{
    DWORD old;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(slot, &mbi, sizeof(mbi))) return 0;
    // Keep execute rights in case the vtable shares a page with code.
    DWORD rw = mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)
                   ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    if (!VirtualProtect(slot, sizeof(void *), rw, &old)) return 0;
    *orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    return 1;
}

// Finds the game's swap chain and patches its Present slots. Runs on the render thread, once.
static void patch_swap_chain(int32_t *deviceIndex)
{
    char mod[MAX_PATH];
    if (!readable(deviceIndex, sizeof(int32_t))) { fail("GpuApi::Present got an unreadable device index", E_POINTER); return; }
    int idx = *deviceIndex;
    uintptr_t dd = readable((void *)g.deviceData, sizeof(void *)) ? *(uintptr_t *)g.deviceData : 0;
    if (!dd) { fail("the game's device data isn't set up", E_POINTER); return; }
    if (idx < 1 || idx > DD_SWAPCHAIN_COUNT) { fail("GpuApi::Present got an unexpected device index", E_INVALIDARG); return; }
    void **entry = (void **)(dd + DD_SWAPCHAINS + (uintptr_t)(idx - 1) * DD_SWAPCHAIN_STRIDE);
    if (!readable(entry, sizeof(void *)) || !looks_like_com(*entry)) {
        fail("the swap chain in the game's device data doesn't look valid (game version changed?)", E_POINTER);
        return;
    }
    IDXGISwapChain *sc = NULL;
    HRESULT hr = IUnknown_QueryInterface((IUnknown *)*entry, &IID_IDXGISwapChain, (void **)&sc);
    if (FAILED(hr)) { fail("QueryInterface(IDXGISwapChain) on the game's swap chain", hr); return; }
    g.vtbl = *(void ***)sc;
    g.scIndex = idx - 1;
    mlog("game swap chain %p (device index %d), Present in %s", (void *)sc, idx, module_of(g.vtbl[VT_PRESENT], mod, sizeof(mod)));
    if (!patch_slot(&g.vtbl[VT_PRESENT], (void *)hook_present, (void **)&g.presentOrig)) {
        IDXGISwapChain_Release(sc);
        fail("patching the swap chain's Present", HRESULT_FROM_WIN32(GetLastError()));
        return;
    }
    IDXGISwapChain1 *sc1 = NULL;
    if (SUCCEEDED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain1, (void **)&sc1))) {
        void **vt1 = *(void ***)sc1;
        if (vt1 == g.vtbl && patch_slot(&g.vtbl[VT_PRESENT1], (void *)hook_present1, (void **)&g.present1Orig))
            mlog("Present1 patched too (in %s)", module_of(g.present1Orig, mod, sizeof(mod)));
        IDXGISwapChain1_Release(sc1);
    }
    // No reference is kept: a later swap chain for the same window (after a display change) needs this one gone,
    // and it shares the patched vtable anyway.
    IDXGISwapChain_Release(sc);
    // Pinned: the patched slots point into this DLL for the rest of the process, so it must never unload.
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCWSTR)(void *)hook_present, &pinned);
    mlog("swap chain Present patched; the squeeze runs %s",
         g.mode == MODE_ON ? "on every frame" : "while a menu is open in 3D Mode");
}

static void *hook_gpu_present(int32_t *deviceIndex, uint8_t sync, UINT interval)
{
    if (!g.patched && !g.failed && InterlockedExchange(&g.patched, 1) == 0) patch_swap_chain(deviceIndex);
    g.gameFrames++;
    // The game can recreate its swap chain at another device index (display or fullscreen change): follow it.
    if (readable(deviceIndex, sizeof(int32_t)) && *deviceIndex >= 1 && *deviceIndex <= DD_SWAPCHAIN_COUNT &&
        *deviceIndex - 1 != g.scIndex) {
        if (g.patched) mlog("game swap chain now at device index %d (was %d)", *deviceIndex, g.scIndex + 1);
        g.scIndex = *deviceIndex - 1;
    }
    if (g.mode == MODE_AUTO) note_game_frame();
    return g.gpuPresentOrig(deviceIndex, sync, interval);
}

// ---------------------------------------------------------------- entry point

int menus_load(void *pluginHandle, const R4eSdk *sdk, const wchar_t *installDir)
{
    InitializeCriticalSection(&g.logLock);
    read_menu_settings(installDir);
    if (g.level > 0) open_hooks_log(installDir);
    mlog("==== 3D Braindance Edition menu squeeze, mode %s, log level %d",
         g.mode == MODE_OFF ? "off" : g.mode == MODE_ON ? "on (every frame)" : "auto", g.level);
    if (g.mode == MODE_OFF) { mlog("[Menus] Squeeze = off: no squeeze hooks installed"); return 1; }
    g.stereo = STEREO_UNKNOWN;
    if (g.mode == MODE_AUTO) read_saved_stereo_setting();
    if (!sdk || !sdk->hooking || !sdk->hooking->Attach) return fail("RED4ext didn't pass a hooking interface", E_POINTER);
    HMODULE r4e = GetModuleHandleW(L"RED4ext.dll");
    ResolveFn resolve = r4e ? (ResolveFn)(void *)GetProcAddress(r4e, "RED4ext_ResolveAddress") : NULL;
    if (!resolve) return fail("RED4ext.dll has no RED4ext_ResolveAddress (RED4ext too old?)", E_NOINTERFACE);
    uintptr_t present = resolve(HASH_GPUAPI_PRESENT);
    g.deviceData = resolve(HASH_DEVICE_DATA);
    mlog("RED4ext resolved GpuApi::Present = %p, device data = %p", (void *)present, (void *)g.deviceData);
    if (!present || !g.deviceData) return fail("RED4ext couldn't resolve the game addresses (game version not in its database?)", E_FAIL);
    if (!sdk->hooking->Attach(pluginHandle, (void *)present, (void *)hook_gpu_present, (void **)&g.gpuPresentOrig))
        return fail("RED4ext couldn't hook GpuApi::Present", E_FAIL);
    mlog("GpuApi::Present hooked; waiting for the first frame");
    // Compiled now, not on the first menu, so opening a menu doesn't stall on the compiler.
    if (!compile_shaders()) return 0;
    return 1;
}
