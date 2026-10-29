// 3D Braindance Edition: lets the game's own DLSS frame generation run in 3D Mode.
//
// The game keeps frame generation off in 3D Mode in two places (static analysis of 2.5, research/native-fg):
//   - the 3D Mode settings handler sets Frame Generation to Off and greys it out whenever 3D Mode is on (it also
//     runs at start-up and on display changes, so a saved "on" doesn't survive);
//   - FlagCompute, which builds each view's render-feature bits every frame, clears feature 88 (DLSS-G) while
//     stereo is on, so the frame graph never feeds DLSS-G.
// On Samsung's monitor that is right: its weaver interlaces the eyes for the tracked eye position, and a generated
// frame would blend two woven pictures. In side by side ([Odyssey3D] SBSDebug = true) there is no weaver, and DLSS-G
// on the whole side-by-side frame works (tested through OptiScaler up to 6x).
//
// Four detours through RED4ext's hooking, the addresses resolved by hash from CDPR's address library (like
// GpuApi::Present in braindance3d_menus.c):
//   - the 3D Mode handler: marks that it is running, and puts Frame Generation's greyed flag back afterwards;
//   - SetFrameGeneration: skips the handler's "Off" while DLSS frame generation is selected. FSR and XeSS frame
//     generation stay blocked in stereo (features 89 and 75), so the game still turns those off as before;
//   - FlagCompute and IsDlssgEnabled: FlagCompute calls IsDlssgEnabled only for a view that passed its other DLSS-G
//     checks, so its answer, noted per thread (FlagCompute runs for several views at once on worker threads), is
//     the game's own DLSS-G decision for that view; on stereo frames feature 88 is set again from it.
// All of them pass straight through unless SBSDebug is on (read on every call: the engine ini may not be applied yet
// when the plugin loads), so a real 3D monitor keeps the game's rule. Before anything is attached, byte patterns in
// each function are checked; if one doesn't match (a game update), nothing is attached and the log says which.
// DLSS-G loads only when Frame Generation is DLSS at game start, so choosing it needs a restart, as in 2D.
//
// License: MIT (see LICENSE).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "braindance3d_menus.h"

// ---------------------------------------------------------------- game addresses (RED4ext hashes)

#define HASH_O3D_CHANGED 224667842u    // 3D Mode settings handler: void (uint8_t *event)
#define HASH_SET_FG 2555059473u        // SetFrameGeneration: uint8_t (int32_t index, uint8_t mode)
#define HASH_FG_MODE 3828097397u       // GetFrameGenerationMode: the selected back end, 1 = DLSS
#define HASH_FLAG_COMPUTE 102052349u   // FlagCompute: void (renderer, uint64_t flags[2], frame, settings)
#define HASH_DLSSG_ENABLED 1336151854u // IsDlssgEnabled: bool (Streamline object)
#define HASH_VAR_SBSDEBUG 3257143555u  // the [Odyssey3D] SBSDebug config var

#define O3D_SIZE 0x560          // the handler's code (pdata 0x2073C00-0x207415F)
#define FLAG_COMPUTE_SIZE 0x1500 // FlagCompute and its chained body (pdata up to 0x227F73A)
#define VAR_NAME 0x08            // config var: const char *name
#define VAR_GROUP 0x10           // config var: const char *group
#define VAR_VALUE 0x30           // config var: the bool value
#define SETTING_FLAGS 0x44       // settings var: flags, FLAG_GREYED = shown greyed out
#define FLAG_GREYED 0x800u
#define FG_MODE_DLSS 1
#define FEATURE_DLSSG 88
#define FEATURE_STEREO 98
#define FEATURE_BIT(n) (1ull << ((n) - 64)) // features 64-127 are in flags[1]

// Byte patterns, each found exactly once in the 2.5 exe ("??" = any byte).
// Start of the 3D Mode handler.
static const char AOB_O3D[] =
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC B8 00 00 00 4C 8B E9 E8";
// In it, the 3D-on branch: xor ecx,ecx then call SetFrameGeneration(0, 1) (the last E8).
static const char AOB_O3D_FG_OFF[] =
    "41 B4 01 E8 ?? ?? ?? ?? 0F B6 D8 E8 ?? ?? ?? ?? 41 0F B6 D4 44 0F B6 F0 33 C9 E8";
// In it, the grey: mov eax,[r15+44h]; bts eax,0Bh.
static const char AOB_O3D_GREY[] = "41 8B 47 44 0F BA E8 0B E9";
// Start of SetFrameGeneration: it finds its settings var as var(group(root(), "/graphics/presets"),
// "FrameGeneration"), the three calls at SET_FG_ROOT, SET_FG_GROUP and SET_FG_VAR.
static const char AOB_SET_FG[] =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 0F B6 F2 8B D9 E8 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 8B C8 "
    "E8 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B F8 85 DB 79 04";
#define SET_FG_ROOT 20         // call root()
#define SET_FG_PRESETS 25      // mov rdx,[CName "/graphics/presets"]
#define SET_FG_GROUP 35        // call group(root, name)
#define SET_FG_NAME 40         // mov rdx,[CName "FrameGeneration"]
#define SET_FG_VAR 50          // call var(group, name)
// Start of FlagCompute.
static const char AOB_FLAG_COMPUTE[] =
    "4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 53 41 54 41 55 41 57 48 83 EC 68 0F 57 C0 4D 8B D1 0F 11 02";
// In it, the DLSS-G check: rcx = renderer->sl; call IsDlssgEnabled; sl->eligible = 1. It's the only call to
// IsDlssgEnabled in FlagCompute (checked at load).
static const char AOB_FLAG_DLSSG[] = "49 8B 89 58 46 00 00 E8 ?? ?? ?? ?? 0F B6 E8 C6 81 F2 01 00 00 01";
#define FLAG_DLSSG_CALL 7
// In it, the stereo clear: feature 88 = dlssg && !stereo.
static const char AOB_FLAG_CLEAR[] = "40 84 ED 74 08 45 84 E4 75 03 41 B0 01 BA 58 00 00 00 E8";

typedef uintptr_t (*ResolveFn)(uint32_t hash);
typedef void (*O3dChangedFn)(uint8_t *event);
typedef uint8_t (*SetFgFn)(int32_t index, uint8_t mode);
typedef uint32_t (*FgModeFn)(void);
typedef void (*FlagComputeFn)(uint8_t *renderer, uint64_t *flags, void *frame, void *settings);
typedef uint8_t (*DlssgEnabledFn)(uint8_t *sl);
typedef void *(*RootFn)(void);
typedef void *(*FindFn)(void *parent, uint64_t cname);

static struct {
    O3dChangedFn o3dOrig;
    SetFgFn setFgOrig;
    FlagComputeFn flagOrig;
    DlssgEnabledFn dlssgOrig;
    FgModeFn fgMode;
    RootFn root;
    FindFn group, var;
    const uint64_t *presetsName, *fgName; // the CNames SetFrameGeneration passes
    const uint8_t *sbsVar;
    DWORD tls;                    // per thread: a T_* state, while FlagCompute runs on that thread
    volatile DWORD handlerThread; // the thread running the 3D Mode handler, 0 when it isn't running
    int kept;                     // that handler call kept Frame Generation
    volatile LONG64 restored;     // frames whose feature 88 was put back
    LONG64 lastRestored;
    DWORD lastSummary;
} fg;

// Per-thread state in fg.tls (a TLS slot, not __thread, which would need libwinpthread with some MinGW builds).
enum { TS_OUTSIDE, TS_NOT_ASKED, TS_DLSSG_OFF, TS_DLSSG_ON }; // TS_OUTSIDE: not inside FlagCompute
static uintptr_t thread_state(void) { return (uintptr_t)TlsGetValue(fg.tls); }
static void set_thread_state(uintptr_t v) { TlsSetValue(fg.tls, (void *)v); }

// ---------------------------------------------------------------- byte patterns

// Parses "48 8B ?? 05" into bytes and a mask (0 for "??"). Returns the length, 0 if malformed or too long.
static int aob_parse(const char *s, uint8_t *bytes, uint8_t *mask, int max)
{
    int n = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (n == max || !s[1]) return 0;
        if (s[0] == '?' && s[1] == '?') {
            bytes[n] = 0;
            mask[n] = 0;
        } else {
            char hex[3] = { s[0], s[1], 0 }, *end;
            unsigned long v = strtoul(hex, &end, 16);
            if (*end) return 0;
            bytes[n] = (uint8_t)v;
            mask[n] = 1;
        }
        n++;
        s += 2;
        if (*s && *s != ' ') return 0;
    }
    return n;
}

static int aob_at(const uint8_t *p, const uint8_t *bytes, const uint8_t *mask, int n)
{
    for (int i = 0; i < n; i++)
        if (mask[i] && p[i] != bytes[i]) return 0;
    return 1;
}

// The offset of the only match of pattern in [p, p + size), or -1 if it isn't there or is there more than once.
static long aob_find_unique(const uint8_t *p, size_t size, const char *pattern)
{
    uint8_t bytes[128], mask[128];
    int n = aob_parse(pattern, bytes, mask, (int)sizeof(bytes));
    if (!n || (size_t)n > size) return -1;
    long found = -1;
    for (size_t i = 0; i + n <= size; i++) {
        if (!aob_at(p + i, bytes, mask, n)) continue;
        if (found >= 0) return -1;
        found = (long)i;
    }
    return found;
}

// The number of bytes in a pattern written as above.
static int aob_len(const char *pattern) { return (int)(strlen(pattern) + 1) / 3; }

// Where an E8 rel32 call goes, and what a 7-byte RIP-relative mov reads.
static uintptr_t call_target(const uint8_t *call) { return (uintptr_t)(call + 5) + *(const int32_t *)(call + 1); }

// How many E8 calls to target there are in [p, p + size).
static int count_calls(const uint8_t *p, size_t size, uintptr_t target)
{
    int n = 0;
    for (size_t i = 0; i + 5 <= size; i++)
        if (p[i] == 0xE8 && call_target(p + i) == target) n++;
    return n;
}
static uintptr_t rip_operand(const uint8_t *insn) { return (uintptr_t)(insn + 7) + *(const int32_t *)(insn + 3); }

// ---------------------------------------------------------------- memory checks

// Whether [p, p + size) lies inside the game exe's image.
static int in_exe(uintptr_t p, size_t size)
{
    const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    uintptr_t lo = (uintptr_t)base, hi = lo + nt->OptionalHeader.SizeOfImage;
    return p >= lo && p < hi && size <= hi - p;
}

static int readable(const void *p, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (const char *)p + size <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

static int text_is(const char *p, const char *want) { return readable(p, strlen(want) + 1) && strcmp(p, want) == 0; }

// ---------------------------------------------------------------- run-time checks

static int sbs_debug(void) { return fg.sbsVar[VAR_VALUE] != 0; }

// Frame Generation's flags, in the settings var found the way SetFrameGeneration finds it (the lookups never return
// NULL for these names; the check is for a var that isn't readable as expected). NULL if that fails.
static uint32_t *fg_flags(void)
{
    uint8_t *var = fg.var(fg.group(fg.root(), *fg.presetsName), *fg.fgName);
    return var && readable(var + SETTING_FLAGS, sizeof(uint32_t)) ? (uint32_t *)(var + SETTING_FLAGS) : NULL;
}

// ---------------------------------------------------------------- hooks

static void hook_o3d_changed(uint8_t *event)
{
    int sbs = sbs_debug();
    plugin_log(1, "framegen: 3D Mode settings handler runs (SBSDebug %s)", sbs ? "on" : "off");
    if (!sbs) {
        fg.o3dOrig(event);
        return;
    }
    uint32_t *flags = fg_flags();
    uint32_t before = flags ? *flags : 0;
    fg.kept = 0;
    fg.handlerThread = GetCurrentThreadId();
    fg.o3dOrig(event);
    fg.handlerThread = 0;
    if (!fg.kept) return;
    if (!flags) {
        plugin_log(1, "framegen: couldn't find the Frame Generation setting, so it may show greyed out");
        return;
    }
    uint32_t after = *flags;
    *flags = (after & ~FLAG_GREYED) | (before & FLAG_GREYED);
    plugin_log(after != *flags ? 1 : 2, "framegen: Frame Generation flags 0x%X -> 0x%X (greyed flag as before 3D Mode)",
               (unsigned)after, (unsigned)*flags);
}

static uint8_t hook_set_fg(int32_t index, uint8_t mode)
{
    if (index == 0 && fg.handlerThread == GetCurrentThreadId()) {
        uint32_t selected = fg.fgMode();
        if (selected == FG_MODE_DLSS) {
            fg.kept = 1;
            plugin_log(1, "framegen: 3D Mode is on, kept DLSS Frame Generation (the game would turn it off)");
            return 1; // what SetFrameGeneration always returns
        }
        plugin_log(1, "framegen: 3D Mode is on and frame generation mode %u isn't DLSS, so the game turns it off",
                   (unsigned)selected);
    }
    return fg.setFgOrig(index, mode);
}

static uint8_t hook_dlssg_enabled(uint8_t *sl)
{
    uint8_t on = fg.dlssgOrig(sl);
    if (thread_state() != TS_OUTSIDE) set_thread_state(on ? TS_DLSSG_ON : TS_DLSSG_OFF);
    return on;
}

static void hook_flag_compute(uint8_t *renderer, uint64_t *flags, void *frame, void *settings)
{
    set_thread_state(TS_NOT_ASKED);
    fg.flagOrig(renderer, flags, frame, settings);
    uintptr_t dlssg = thread_state();
    set_thread_state(TS_OUTSIDE);
    // The original's own DLSS-G decision for this view, without its "and not stereo".
    if (dlssg != TS_DLSSG_ON || !(flags[1] & FEATURE_BIT(FEATURE_STEREO)) || (flags[1] & FEATURE_BIT(FEATURE_DLSSG)) ||
        !sbs_debug())
        return;
    flags[1] |= FEATURE_BIT(FEATURE_DLSSG);
    LONG64 n = InterlockedIncrement64(&fg.restored);
    if (n == 1) plugin_log(1, "framegen: DLSS frame generation running on a 3D frame (first one)");
    DWORD now = GetTickCount();
    if (now - fg.lastSummary >= 60000) {
        plugin_log(1, "framegen: %lld frames with DLSS frame generation in 3D (%lld in the last minute)", (long long)n,
                   (long long)(n - fg.lastRestored));
        fg.lastSummary = now;
        fg.lastRestored = n;
    }
}

// ---------------------------------------------------------------- set-up

static int fail(const char *what)
{
    plugin_log(1, "framegen: %s (game update?); nothing attached, 3D Mode keeps the game's frame generation rule", what);
    return 0;
}

enum { T_SET_FG, T_O3D, T_DLSSG, T_FLAG_COMPUTE, T_COUNT }; // in attach order

// Checks every address and pattern before anything is attached, and fills t[T_*] with the hook targets.
static int find_targets(ResolveFn resolve, uint8_t **t)
{
    uint8_t **o3d = &t[T_O3D], **setFg = &t[T_SET_FG], **flagCompute = &t[T_FLAG_COMPUTE];
    *o3d = (uint8_t *)resolve(HASH_O3D_CHANGED);
    *setFg = (uint8_t *)resolve(HASH_SET_FG);
    *flagCompute = (uint8_t *)resolve(HASH_FLAG_COMPUTE);
    uint8_t *fgMode = (uint8_t *)resolve(HASH_FG_MODE);
    uint8_t *dlssg = t[T_DLSSG] = (uint8_t *)resolve(HASH_DLSSG_ENABLED);
    uint8_t *sbs = (uint8_t *)resolve(HASH_VAR_SBSDEBUG);
    plugin_log(1, "framegen: resolved 3D Mode handler %p, SetFrameGeneration %p, GetFrameGenerationMode %p, "
                  "FlagCompute %p, IsDlssgEnabled %p, SBSDebug var %p",
               (void *)*o3d, (void *)*setFg, (void *)fgMode, (void *)*flagCompute, (void *)dlssg, (void *)sbs);
    if (!in_exe((uintptr_t)*o3d, O3D_SIZE) || !in_exe((uintptr_t)*setFg, 64) || !in_exe((uintptr_t)fgMode, 16) ||
        !in_exe((uintptr_t)*flagCompute, FLAG_COMPUTE_SIZE) || !in_exe((uintptr_t)dlssg, 16) ||
        !in_exe((uintptr_t)sbs, VAR_VALUE + 1))
        return fail("an address didn't resolve into the game exe");

    if (aob_find_unique(*o3d, O3D_SIZE, AOB_O3D) != 0) return fail("3D Mode handler: start doesn't match");
    long off = aob_find_unique(*o3d, O3D_SIZE, AOB_O3D_FG_OFF);
    if (off < 0) return fail("3D Mode handler: the Frame Generation Off call isn't there");
    const uint8_t *call = *o3d + off + aob_len(AOB_O3D_FG_OFF) - 1;
    if (call_target(call) != (uintptr_t)*setFg) return fail("3D Mode handler: the Off call doesn't go to SetFrameGeneration");
    if (aob_find_unique(*o3d, O3D_SIZE, AOB_O3D_GREY) < 0) return fail("3D Mode handler: the greyed flag isn't set where expected");
    plugin_log(2, "framegen: 3D Mode handler checked (Off call at +0x%lX)", (long)(call - *o3d));

    if (aob_find_unique(*setFg, 64, AOB_SET_FG) != 0) return fail("SetFrameGeneration: start doesn't match");
    uintptr_t root = call_target(*setFg + SET_FG_ROOT), group = call_target(*setFg + SET_FG_GROUP),
              var = call_target(*setFg + SET_FG_VAR), presets = rip_operand(*setFg + SET_FG_PRESETS),
              name = rip_operand(*setFg + SET_FG_NAME);
    if (!in_exe(root, 1) || !in_exe(group, 1) || !in_exe(var, 1) || !in_exe(presets, 8) || !in_exe(name, 8))
        return fail("SetFrameGeneration: its settings lookup points outside the exe");

    if (aob_find_unique(*flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_COMPUTE) != 0) return fail("FlagCompute: start doesn't match");
    off = aob_find_unique(*flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_DLSSG);
    if (off < 0) return fail("FlagCompute: the DLSS-G check isn't there");
    if (call_target(*flagCompute + off + FLAG_DLSSG_CALL) != (uintptr_t)dlssg)
        return fail("FlagCompute: the DLSS-G check doesn't call IsDlssgEnabled");
    if (count_calls(*flagCompute, FLAG_COMPUTE_SIZE, (uintptr_t)dlssg) != 1)
        return fail("FlagCompute: IsDlssgEnabled is called somewhere else in it too");
    long clear = aob_find_unique(*flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_CLEAR);
    if (clear < 0) return fail("FlagCompute: the stereo clear of feature 88 isn't there");
    plugin_log(2, "framegen: FlagCompute checked (DLSS-G check at +0x%lX, stereo clear at +0x%lX)", off, clear);

    const char *const *names = (const char *const *)sbs;
    if (!text_is(names[VAR_NAME / 8], "SBSDebug") || !text_is(names[VAR_GROUP / 8], "Odyssey3D"))
        return fail("the SBSDebug config var isn't named [Odyssey3D] SBSDebug");

    fg.fgMode = (FgModeFn)(void *)fgMode;
    fg.root = (RootFn)root;
    fg.group = (FindFn)group;
    fg.var = (FindFn)var;
    fg.presetsName = (const uint64_t *)presets;
    fg.fgName = (const uint64_t *)name;
    fg.sbsVar = sbs;
    plugin_log(1, "framegen: all checks passed (SBSDebug is %s right now)", sbs_debug() ? "on" : "off");
    return 1;
}

// All four hooks or none.
static int attach_hooks(void *pluginHandle, R4eHooking *h, ResolveFn resolve)
{
    uint8_t *t[T_COUNT];
    if (!find_targets(resolve, t)) return 0;
    if ((fg.tls = TlsAlloc()) == TLS_OUT_OF_INDEXES) return fail("no thread-local storage slot left");
    static const char *const names[T_COUNT] = { "SetFrameGeneration", "the 3D Mode handler", "IsDlssgEnabled", "FlagCompute" };
    void *const detours[T_COUNT] = { (void *)hook_set_fg, (void *)hook_o3d_changed, (void *)hook_dlssg_enabled,
                                     (void *)hook_flag_compute };
    void **const origs[T_COUNT] = { (void **)&fg.setFgOrig, (void **)&fg.o3dOrig, (void **)&fg.dlssgOrig,
                                    (void **)&fg.flagOrig };
    for (int i = 0; i < T_COUNT; i++) {
        if (h->Attach(pluginHandle, t[i], detours[i], origs[i])) continue;
        plugin_log(1, "framegen: couldn't hook %s", names[i]);
        while (i-- > 0) h->Detach(pluginHandle, t[i]);
        return fail("a hook couldn't be attached");
    }
    plugin_log(1, "framegen: hooks attached; with SBSDebug on, DLSS frame generation stays available in 3D Mode");
    return 1;
}

int framegen_load(void *pluginHandle, const R4eSdk *sdk)
{
    if (!sdk || !sdk->hooking || !sdk->hooking->Attach || !sdk->hooking->Detach)
        return fail("RED4ext didn't pass a hooking interface");
    HMODULE r4e = GetModuleHandleW(L"RED4ext.dll");
    ResolveFn resolve = r4e ? (ResolveFn)(void *)GetProcAddress(r4e, "RED4ext_ResolveAddress") : NULL;
    if (!resolve) return fail("RED4ext.dll has no RED4ext_ResolveAddress");
    return attach_hooks(pluginHandle, sdk->hooking, resolve);
}
