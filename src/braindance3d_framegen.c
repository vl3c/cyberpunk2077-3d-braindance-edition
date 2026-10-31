// 3D Braindance Edition: lets the game's own DLSS and FSR (3 and 4) frame generation run in 3D Mode.
//
// The game keeps frame generation off in 3D Mode in two places (static analysis of 2.5, research/native-fg):
//   - the 3D Mode settings handler sets Frame Generation to Off and greys it out whenever 3D Mode is on (it also
//     runs at start-up and on display changes, so a saved "on" doesn't survive);
//   - FlagCompute, which builds each view's render-feature bits every frame, clears feature 88 (DLSS-G) and 89
//     (FSR frame generation) while stereo is on, so the frame graph never feeds them.
// On Samsung's monitor that is right: its weaver interlaces the eyes for the tracked eye position, and a generated
// frame would blend two woven pictures. In side by side ([Odyssey3D] SBSDebug = true) there is no weaver, and DLSS-G
// on the whole side-by-side frame works (tested through OptiScaler up to 6x), and so does the game's FSR 3.1 frame
// generation (tested in game; FSR4 untested).
//
// Four detours through RED4ext's hooking, the addresses resolved by hash from CDPR's address library (like
// GpuApi::Present in braindance3d_menus.c):
//   - the 3D Mode handler: marks that it is running, and clears Frame Generation's greyed flag afterwards. It
//     also tells the menu squeeze which way 3D Mode was set (whether the handler asked for Off);
//   - SetFrameGeneration: skips the handler's "Off" (and so the grey) when Frame Generation is DLSS (on a PC that
//     offers DLSS frame generation), FSR3, FSR4 (the AI frame generation of AMD's RX 9000, untested), or Off while one
//     of those can be picked, so it keeps its setting and can be chosen in 3D Mode. XeSS stays blocked (its feature 75 isn't put back), so with it
//     or a value this doesn't know selected, the game still turns it off and greys it out;
//   - FlagCompute and IsDlssgEnabled: FlagCompute calls IsDlssgEnabled only for a view that passed its other DLSS-G
//     checks, so its answer, noted per thread (FlagCompute runs for several views at once on worker threads), is
//     the game's own DLSS-G decision for that view; on stereo frames feature 88 is set again from it. FSR frame
//     generation has the same view checks, in the same place, so a view that was asked about DLSS-G is one the
//     game also asked IsFsrFgEnabled about: for it, feature 89 is set again from that function's answer (a read of
//     two flags, called by us rather than hooked).
// Apart from telling the menu squeeze about 3D Mode, they change nothing unless SBSDebug is on (read on every call:
// the engine ini isn't applied yet when the plugin loads), so a real 3D monitor keeps the game's rule. Each is entered through a thunk that keeps the
// caller's registers (see REGISTER_KEEPING_THUNK). Before anything is attached, the addresses and byte patterns are
// checked (check_handler, check_framegen); if one doesn't match (a game update), that group isn't attached and the log
// says which: without the frame generation group the game keeps its rule, and without the handler group the menu squeeze
// also loses the 3D Mode switches. If only the FSR part doesn't match (check_fsr), DLSS still works and FSR keeps the
// game's rule.
// DLSS-G loads only when Frame Generation is DLSS at game start, so choosing it needs a restart, as in 2D; so does FSR.
// With FSR at start the game presents through FidelityFX's frame generation swap chain.
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
#define HASH_FLAG_COMPUTE 102052349u   // FlagCompute: uint64_t *(renderer, uint64_t flags[2], frame, settings), returns flags
#define HASH_DLSSG_ENABLED 1336151854u // IsDlssgEnabled: bool (Streamline object)
#define HASH_VAR_SBSDEBUG 3257143555u  // the [Odyssey3D] SBSDebug config var

#define O3D_SIZE 0x560          // the handler's code (pdata 0x2073C00-0x207415F)
#define FLAG_COMPUTE_SIZE 0x116A // FlagCompute and its chained body (pdata 0x227E5D0 up to 0x227F73A)
#define VAR_NAME 0x08            // config var: const char *name
#define VAR_GROUP 0x10           // config var: const char *group
#define VAR_VALUE 0x30           // config var: the bool value
// Frame Generation's settings var (a list setting whose values the game fills in at run time).
#define SETTING_FLAGS 0x44       // flags:
#define FLAG_FROM_FILE 0x20u     //   SETTING_FILE_VALUE holds the value read from UserSettings.json (list not built yet)
#define FLAG_LIST_BUILT 0x40u    //   SETTING_VALUES is filled, SETTING_PENDING indexes it
#define FLAG_GREYED 0x800u       //   shown greyed out
#define SETTING_FILE_VALUE 0x58  // CString
#define SETTING_APPLIED 0x78     // int32: applied index
#define SETTING_PENDING 0x84     // int32: index the menu shows (what SetFrameGeneration writes)
#define SETTING_VALUES 0x90      // CString *entries at +0, uint32 size at +0xC: "Off", "DLSS", "FSR3", "FSR4", "XESS"
#define SETTING_SIZE 0xA0        //   (only those the PC supports, so indices differ between PCs)
#define CSTRING_SIZE 0x20        // CString: inline text at +0, or a heap pointer at +0 if CSTRING_HEAP;
#define CSTRING_LEN_FLAGS 0x14   //   the length in the low 30 bits of the uint32 at +0x14
#define CSTRING_HEAP 0x40000000u
#define FEATURE_DLSSG 88
#define FEATURE_FSR_FG 89
#define RENDERER_FSR 0x4668      // the renderer's FSR object, what FlagCompute passes IsFsrFgEnabled
#define FSR_ENABLED_SIZE 18      // IsFsrFgEnabled's code
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
// In it, after the DLSS-G check and behind the same view checks, the FSR frame generation check:
// rdx = renderer (FlagCompute's first argument, saved at [rsp+90h]); r11b = IsFsrFgEnabled(rdx->fsr).
static const char AOB_FLAG_FSR[] = "48 8B 94 24 90 00 00 00 48 8B 8A 68 46 00 00 E8 ?? ?? ?? ?? 44 0F B6 D8";
#define FLAG_FSR_CALL 15
// IsFsrFgEnabled: bool (FSR object): fsr[0x59] && !fsr[0x5A]. Only reads, so it's safe to call on any thread.
static const char AOB_FSR_ENABLED[] = "80 79 59 00 74 09 80 79 5A 00 75 03 B0 01 C3 32 C0 C3";
// In FlagCompute, after the clear of 88, the one of feature 89: fsr && !stereo.
static const char AOB_FLAG_CLEAR_FSR[] = "45 84 DB 74 0A 45 84 E4 75 05 41 B0 01 EB 03 45 33 C0 BA 59 00 00 00 E8";

typedef uintptr_t (*ResolveFn)(uint32_t hash);
typedef void (*O3dChangedFn)(uint8_t *event);
typedef uint8_t (*SetFgFn)(int32_t index, uint8_t mode);
typedef uint64_t *(*FlagComputeFn)(uint8_t *renderer, uint64_t *flags, void *frame, void *settings);
typedef uint8_t (*DlssgEnabledFn)(uint8_t *sl);
typedef uint8_t (*FsrFgEnabledFn)(const uint8_t *fsr);
typedef void *(*RootFn)(void);
typedef void *(*FindFn)(void *parent, uint64_t cname);

// Frames whose frame generation feature was put back, for the log.
typedef struct {
    volatile LONG64 restored;
    LONG64 lastRestored;
    DWORD lastSummary;
} RestoreCount;

static struct {
    O3dChangedFn o3dOrig;
    SetFgFn setFgOrig;
    FlagComputeFn flagOrig;
    DlssgEnabledFn dlssgOrig;
    FsrFgEnabledFn fsrEnabled;    // the game's IsFsrFgEnabled; NULL if its checks failed (FSR keeps the game's rule)
    RootFn root;
    FindFn group, var;
    const uint64_t *presetsName, *fgName; // the CNames SetFrameGeneration passes
    const uint8_t *sbsVar;
    int framegen;                 // the frame generation hooks are attached (not only the handler's)
    DWORD tls;                    // per thread: a T_* state, while FlagCompute runs on that thread
    volatile DWORD handlerThread; // the thread running the 3D Mode handler, 0 when it isn't running
    int offAsked;                 // that handler call asked for Frame Generation Off (its 3D-on branch)
    uint32_t *keptFlags;          // and it was kept: Frame Generation's flags, NULL if not kept
    RestoreCount dlssgCount, fsrCount;
} fg;

// Per-thread state in fg.tls (a TLS slot, not __thread, which would need libwinpthread with some MinGW builds).
// TS_OUTSIDE: not inside FlagCompute; TS_NOT_ASKED: inside, and the view didn't pass the frame generation checks (so
// far); TS_DLSSG_OFF / ON: it did, and IsDlssgEnabled answered no / yes.
enum { TS_OUTSIDE, TS_NOT_ASKED, TS_DLSSG_OFF, TS_DLSSG_ON };
// Both keep the thread's last-error value (TlsGetValue clears it), which the game's code around the hooks may use.
static uintptr_t thread_state(void)
{
    DWORD err = GetLastError();
    uintptr_t v = (uintptr_t)TlsGetValue(fg.tls);
    SetLastError(err);
    return v;
}

static void set_thread_state(uintptr_t v)
{
    DWORD err = GetLastError();
    TlsSetValue(fg.tls, (void *)v);
    SetLastError(err);
}

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

// Frame Generation's settings var, found the way SetFrameGeneration finds it. NULL if it isn't readable as expected.
static uint8_t *fg_var(void)
{
    uint8_t *var = fg.var(fg.group(fg.root(), *fg.presetsName), *fg.fgName);
    return readable(var, SETTING_SIZE) ? var : NULL;
}

// Copies a CString into out (NUL-terminated). 0 if it isn't readable or doesn't fit.
static int read_cstring(const uint8_t *s, char *out, size_t size)
{
    if (!readable(s, CSTRING_SIZE)) return 0;
    uint32_t lenFlags = *(const uint32_t *)(s + CSTRING_LEN_FLAGS), len = lenFlags & 0x3FFFFFFFu;
    const char *text = lenFlags & CSTRING_HEAP ? *(const char *const *)s : (const char *)s;
    if (len >= size || (len && !readable(text, len))) return 0;
    memcpy(out, text, len);
    out[len] = 0;
    return 1;
}

// The Frame Generation value the menu shows ("Off", "DLSS", "FSR3", "FSR4" or "XESS"), logged with the var's state.
// 0 if it can't be read. (The game's own GetFrameGenerationMode reads only the value from the file, which the game
// empties once the list is built: it answers "unknown" from then on.)
static int fg_value(const uint8_t *var, char *out, size_t size)
{
    uint32_t flags = *(const uint32_t *)(var + SETTING_FLAGS);
    int32_t pending = *(const int32_t *)(var + SETTING_PENDING), applied = *(const int32_t *)(var + SETTING_APPLIED);
    const uint8_t *values = *(const uint8_t *const *)(var + SETTING_VALUES);
    uint32_t count = *(const uint32_t *)(var + SETTING_VALUES + 0xC);
    int ok = 0;
    if (flags & FLAG_LIST_BUILT)
        ok = pending >= 0 && (uint32_t)pending < count && count <= 16 &&
             read_cstring(values + (size_t)pending * CSTRING_SIZE, out, size);
    else if (flags & FLAG_FROM_FILE)
        ok = read_cstring(var + SETTING_FILE_VALUE, out, size);
    plugin_log(1, "framegen: Frame Generation setting: flags 0x%X, index %d (applied %d) of %u, value '%s'",
               (unsigned)flags, (int)pending, (int)applied, (unsigned)count, ok ? out : "?");
    return ok;
}

// Whether the setting's list offers this value (DLSS: an RTX 40 or 50; FSR3: every PC). Before the list is built
// (at start-up) it can't tell, and the value from the file decides alone.
static int offers(const uint8_t *var, const char *name)
{
    if (!(*(const uint32_t *)(var + SETTING_FLAGS) & FLAG_LIST_BUILT)) return 1;
    const uint8_t *values = *(const uint8_t *const *)(var + SETTING_VALUES);
    uint32_t count = *(const uint32_t *)(var + SETTING_VALUES + 0xC);
    char value[32];
    for (uint32_t i = 0; i < count && count <= 16; i++)
        if (read_cstring(values + (size_t)i * CSTRING_SIZE, value, sizeof(value)) && strcmp(value, name) == 0)
            return 1;
    return 0;
}

// The values whose frame generation runs in 3D Mode with these hooks: DLSS, and FSR3 and FSR4 if the FSR checks passed.
static int is_fsr(const char *value) { return strcmp(value, "FSR3") == 0 || strcmp(value, "FSR4") == 0; }

static int runs_in_3d(const uint8_t *var, const char *value)
{
    if (strcmp(value, "DLSS") == 0) return offers(var, "DLSS");
    if (is_fsr(value)) return fg.fsrEnabled && offers(var, value);
    return 0;
}

// Whether 3D Mode may keep this Frame Generation setting: one that runs in 3D, or Off while one of those can be
// chosen (FSR3 is on every list that has FSR4). XeSS frame generation stays blocked in stereo, and so does a value this
// doesn't know (a game update's new one), so the game's rule applies to those: Off, greyed out. A value that can't be
// read counts as Off.
static int keep_in_3d(const uint8_t *var, const char *value, int known)
{
    if (!known || strcmp(value, "Off") == 0) return runs_in_3d(var, "DLSS") || runs_in_3d(var, "FSR3");
    return runs_in_3d(var, value);
}

// ---------------------------------------------------------------- hooks

// The game is built with MSVC's whole-program optimiser, which lets a caller keep using a volatile register across a
// call when it knows the callee leaves it alone: FlagCompute reads rcx again after calling IsDlssgEnabled. C code
// (ours, kernel32's) doesn't keep such promises, so every hook is entered through this thunk. It saves rcx, rdx,
// r8-r11 and xmm0-xmm5 around the C function and returns its rax; the arguments stay in their registers (no hooked
// function takes more than four). The unwind info covers the pushes and the stack allocation. Only the low 128 bits of
// the xmm registers are kept: the game's code around the hooks is SSE (no VEX), which doesn't keep the upper ones.
#define REGISTER_KEEPING_THUNK(thunk, impl)                                                                         \
    __asm__(".text\n.p2align 4\n.globl " #thunk "\n.def " #thunk "; .scl 2; .type 32; .endef\n"                  \
            ".seh_proc " #thunk "\n" #thunk ":\n"                                                                   \
            "push %rcx\n.seh_pushreg %rcx\npush %rdx\n.seh_pushreg %rdx\npush %r8\n.seh_pushreg %r8\n"             \
            "push %r9\n.seh_pushreg %r9\npush %r10\n.seh_pushreg %r10\npush %r11\n.seh_pushreg %r11\n"             \
            "sub $0x88, %rsp\n.seh_stackalloc 0x88\n.seh_endprologue\n" /* rsp is 16-aligned here */             \
            "movdqu %xmm0, 0x20(%rsp)\nmovdqu %xmm1, 0x30(%rsp)\nmovdqu %xmm2, 0x40(%rsp)\n"                      \
            "movdqu %xmm3, 0x50(%rsp)\nmovdqu %xmm4, 0x60(%rsp)\nmovdqu %xmm5, 0x70(%rsp)\n"                      \
            "call " #impl "\n"                                                                                      \
            "movdqu 0x20(%rsp), %xmm0\nmovdqu 0x30(%rsp), %xmm1\nmovdqu 0x40(%rsp), %xmm2\n"                      \
            "movdqu 0x50(%rsp), %xmm3\nmovdqu 0x60(%rsp), %xmm4\nmovdqu 0x70(%rsp), %xmm5\n"                      \
            "add $0x88, %rsp\npop %r11\npop %r10\npop %r9\npop %r8\npop %rdx\npop %rcx\nret\n.seh_endproc\n")

#define HOOK __attribute__((used, noinline, noclone)) static // called by name from its thunk

// Clears Frame Generation's greyed flag. In the game's code only the 3D Mode handler greys it, so with a setting kept
// it's choosable again (also after an earlier run greyed it for XeSS).
static void ungrey(uint32_t *flags)
{
    uint32_t before = *flags;
    *flags = before & ~FLAG_GREYED;
    plugin_log(before != *flags ? 1 : 2, "framegen: Frame Generation flags 0x%X -> 0x%X (not greyed)", (unsigned)before,
               (unsigned)*flags);
}

// Runs the game's handler with this thread marked as running it. *offAsked: it asked for Frame Generation Off;
// *kept: the flags of the Frame Generation setting that was kept (NULL if none). The marks are saved and put back, in
// case the game ever runs the handler from inside itself.
static void run_handler(uint8_t *event, int *offAsked, uint32_t **kept)
{
    int outerOffAsked = fg.offAsked;
    uint32_t *outerKept = fg.keptFlags;
    DWORD outerThread = fg.handlerThread;
    fg.offAsked = 0;
    fg.keptFlags = NULL;
    fg.handlerThread = GetCurrentThreadId();
    fg.o3dOrig(event);
    *offAsked = fg.offAsked;
    *kept = fg.keptFlags;
    fg.offAsked = outerOffAsked;
    fg.keptFlags = outerKept;
    fg.handlerThread = outerThread;
}

HOOK void hook_o3d_changed(uint8_t *event)
{
    // event[1] is 1 when 3D Mode was switched in the menu, 0 at start-up and on a display change. Read before the
    // handler runs: it clears it on the way out.
    int ev1 = readable(event, 2) ? event[1] : -1;
    int offAsked;
    uint32_t *flags;
    run_handler(event, &offAsked, &flags);
    // Its branch for "3D Mode on and the 3D display available" starts with its only SetFrameGeneration call (checked at
    // load), and it has no other way out than that branch or the 3D-off one, so the call tells which way it set 3D
    // Mode. The menu squeeze follows it.
    plugin_log(1, "framegen: 3D Mode settings handler ran (event %d): 3D Mode %s (SBSDebug %s)", ev1,
               offAsked ? "on" : "off", !fg.framegen ? "not checked" : sbs_debug() ? "on" : "off");
    menus_note_3d_mode(offAsked);
    if (flags) ungrey(flags);
}

// Whether this SetFrameGeneration call is the 3D Mode handler's own "Off": (0, 1), on its thread, while it runs.
static int is_handlers_off(int32_t index, uint8_t mode)
{
    return index == 0 && mode == 1 && fg.handlerThread == GetCurrentThreadId();
}

// Whether to skip the handler's Off (and so its grey) and keep Frame Generation as it is; then fg.keptFlags says
// which flags to ungrey after the handler.
static int keep_frame_generation(void)
{
    if (!fg.framegen || !sbs_debug()) return 0; // the game's rule (a real 3D monitor)
    uint8_t *var = fg_var();
    if (!var) {
        plugin_log(1, "framegen: couldn't find the Frame Generation setting, so the game turns it off in 3D Mode");
        return 0;
    }
    char value[32];
    int known = fg_value(var, value, sizeof(value));
    if (!keep_in_3d(var, value, known)) {
        const char *why = "";
        if (!known || strcmp(value, "Off") == 0 || strcmp(value, "DLSS") == 0) {
            if (!offers(var, "DLSS"))
                why = fg.fsrEnabled ? " (the game offers neither DLSS frame generation nor FSR3)"
                                    : " (the game offers no DLSS frame generation: not an RTX 40 or 50, or another mod "
                                      "such as OptiScaler holds DLSS-G; and the FSR checks at start-up failed)";
        } else if (is_fsr(value) && !fg.fsrEnabled)
            why = " (the FSR checks at start-up failed, see above)";
        plugin_log(1, "framegen: 3D Mode is on and Frame Generation is %s, which can't run in 3D, so the game turns it "
                      "off%s", known ? value : "?", why);
        return 0;
    }
    fg.keptFlags = (uint32_t *)(var + SETTING_FLAGS); // the grey comes later in the handler: undone after it
    plugin_log(1, "framegen: 3D Mode is on, Frame Generation kept and choosable (the game would turn it off)");
    return 1;
}

HOOK uint8_t hook_set_fg(int32_t index, uint8_t mode)
{
    if (is_handlers_off(index, mode)) {
        fg.offAsked = 1;
        if (keep_frame_generation()) return 1; // what SetFrameGeneration always returns
    }
    return fg.setFgOrig(index, mode);
}

HOOK uint8_t hook_dlssg_enabled(uint8_t *sl)
{
    uint8_t on = fg.dlssgOrig(sl);
    if (thread_state() != TS_OUTSIDE) set_thread_state(on ? TS_DLSSG_ON : TS_DLSSG_OFF);
    return on;
}

// Whether a stereo view's feature goes back on: it isn't set, so only the original's "and not stereo" can have taken
// it away, and the game's own decision for the view said yes.
static int stereo_view_without(uint64_t features, int feature)
{
    return (features & FEATURE_BIT(FEATURE_STEREO)) && !(features & FEATURE_BIT(feature)) && sbs_debug();
}

// Feature 88: the original's DLSS-G decision for the view (state, a TS_* value) said yes.
static int should_restore_dlssg(uintptr_t state, uint64_t features)
{
    return state == TS_DLSSG_ON && stereo_view_without(features, FEATURE_DLSSG);
}

// Whether the game's IsFsrFgEnabled says yes for this renderer's FSR object (FSR3 or FSR4 frame generation: the FSR
// object turns it on only while the mode chosen is the one the game started with).
static int fsr_fg_enabled(const uint8_t *renderer)
{
    const uint8_t *fsr = renderer ? *(const uint8_t *const *)(renderer + RENDERER_FSR) : NULL;
    return fsr && fg.fsrEnabled(fsr);
}

// Feature 89: the view passed the frame generation checks (the original asked about DLSS-G, so it also asked
// IsFsrFgEnabled), and IsFsrFgEnabled says yes. Asked last: it's a call. The checks before IsDlssgEnabled don't depend
// on DLSS-G being there, and the Streamline object always exists (FlagCompute writes to it either way), so a PC without
// DLSS-G (AMD, Intel) is asked too.
static int should_restore_fsr(uintptr_t state, uint64_t features, const uint8_t *renderer)
{
    return fg.fsrEnabled && (state == TS_DLSSG_OFF || state == TS_DLSSG_ON) &&
           stereo_view_without(features, FEATURE_FSR_FG) && fsr_fg_enabled(renderer);
}

// Counts a restored frame; logs the first one and then a count once a minute.
static void count_restored(RestoreCount *c, const char *name)
{
    LONG64 n = InterlockedIncrement64(&c->restored);
    DWORD now = GetTickCount();
    if (n == 1) {
        plugin_log(1, "framegen: %s frame generation running on a 3D frame (first one)", name);
        c->lastSummary = now;
    } else if (now - c->lastSummary >= 60000) { // from several threads: a count can be off by a few, harmless
        plugin_log(1, "framegen: %lld frames with %s frame generation in 3D (%lld in the last minute)", (long long)n,
                   name, (long long)(n - c->lastRestored));
        c->lastSummary = now;
        c->lastRestored = n;
    }
}

// Returns what the original returns (the flags): its caller reads the bits through it.
HOOK uint64_t *hook_flag_compute(uint8_t *renderer, uint64_t *flags, void *frame, void *settings)
{
    uintptr_t outer = thread_state(); // TS_OUTSIDE, unless FlagCompute ever runs inside itself
    set_thread_state(TS_NOT_ASKED);
    uint64_t *ret = fg.flagOrig(renderer, flags, frame, settings);
    uintptr_t state = thread_state();
    set_thread_state(outer);
    uint64_t *bits = ret ? ret : flags;
    if (should_restore_dlssg(state, bits[1])) {
        bits[1] |= FEATURE_BIT(FEATURE_DLSSG);
        count_restored(&fg.dlssgCount, "DLSS");
    }
    if (should_restore_fsr(state, bits[1], renderer)) {
        bits[1] |= FEATURE_BIT(FEATURE_FSR_FG);
        count_restored(&fg.fsrCount, "FSR");
    }
    return ret;
}

REGISTER_KEEPING_THUNK(bd3d_thunk_o3d_changed, hook_o3d_changed);
REGISTER_KEEPING_THUNK(bd3d_thunk_set_fg, hook_set_fg);
REGISTER_KEEPING_THUNK(bd3d_thunk_dlssg_enabled, hook_dlssg_enabled);
REGISTER_KEEPING_THUNK(bd3d_thunk_flag_compute, hook_flag_compute);
void bd3d_thunk_o3d_changed(uint8_t *event);
uint8_t bd3d_thunk_set_fg(int32_t index, uint8_t mode);
uint8_t bd3d_thunk_dlssg_enabled(uint8_t *sl);
uint64_t *bd3d_thunk_flag_compute(uint8_t *renderer, uint64_t *flags, void *frame, void *settings);

// ---------------------------------------------------------------- set-up

// Logs why a check failed (a game update, most likely). Returns 0 so callers can `return fail(...)`.
static int fail(const char *what)
{
    plugin_log(1, "framegen: %s (game update?)", what);
    return 0;
}

enum { T_SET_FG, T_O3D, T_DLSSG, T_FLAG_COMPUTE, T_COUNT }; // in attach order

// The 3D Mode handler and its SetFrameGeneration call: enough to see which way 3D Mode is set (for the menu squeeze).
static int check_handler(ResolveFn resolve, uint8_t **t)
{
    uint8_t *o3d = t[T_O3D] = (uint8_t *)resolve(HASH_O3D_CHANGED);
    uint8_t *setFg = t[T_SET_FG] = (uint8_t *)resolve(HASH_SET_FG);
    plugin_log(1, "framegen: resolved 3D Mode handler %p, SetFrameGeneration %p", (void *)o3d, (void *)setFg);
    if (!in_exe((uintptr_t)o3d, O3D_SIZE) || !in_exe((uintptr_t)setFg, 64))
        return fail("3D Mode handler or SetFrameGeneration didn't resolve into the game exe");
    if (aob_find_unique(o3d, O3D_SIZE, AOB_O3D) != 0) return fail("3D Mode handler: start doesn't match");
    long off = aob_find_unique(o3d, O3D_SIZE, AOB_O3D_FG_OFF);
    if (off < 0) return fail("3D Mode handler: the Frame Generation Off call isn't there");
    const uint8_t *call = o3d + off + aob_len(AOB_O3D_FG_OFF) - 1;
    if (call_target(call) != (uintptr_t)setFg) return fail("3D Mode handler: the Off call doesn't go to SetFrameGeneration");
    if (count_calls(o3d, O3D_SIZE, (uintptr_t)setFg) != 1)
        return fail("3D Mode handler: it calls SetFrameGeneration somewhere else too");
    plugin_log(2, "framegen: 3D Mode handler checked (Off call at +0x%lX)", (long)(call - o3d));
    return 1;
}

// How SetFrameGeneration finds its settings var: var(group(root(), *presets), *name).
typedef struct {
    uintptr_t root, group, var, presets, name;
} SettingsLookup;

// SetFrameGeneration's start, and its settings lookup (into l) pointing into the exe.
static int check_settings_lookup(const uint8_t *setFg, SettingsLookup *l)
{
    if (aob_find_unique(setFg, 64, AOB_SET_FG) != 0) return fail("SetFrameGeneration: start doesn't match");
    l->root = call_target(setFg + SET_FG_ROOT);
    l->group = call_target(setFg + SET_FG_GROUP);
    l->var = call_target(setFg + SET_FG_VAR);
    l->presets = rip_operand(setFg + SET_FG_PRESETS);
    l->name = rip_operand(setFg + SET_FG_NAME);
    if (!in_exe(l->root, 1) || !in_exe(l->group, 1) || !in_exe(l->var, 1) || !in_exe(l->presets, 8) || !in_exe(l->name, 8))
        return fail("SetFrameGeneration: its settings lookup points outside the exe");
    return 1;
}

// FlagCompute's start, its DLSS-G check (the only call to dlssg in it) and the stereo clear of feature 88 after it.
static int check_flag_compute(const uint8_t *flagCompute, const uint8_t *dlssg)
{
    if (aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_COMPUTE) != 0) return fail("FlagCompute: start doesn't match");
    long off = aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_DLSSG);
    if (off < 0) return fail("FlagCompute: the DLSS-G check isn't there");
    if (call_target(flagCompute + off + FLAG_DLSSG_CALL) != (uintptr_t)dlssg)
        return fail("FlagCompute: the DLSS-G check doesn't call IsDlssgEnabled");
    if (count_calls(flagCompute, FLAG_COMPUTE_SIZE, (uintptr_t)dlssg) != 1)
        return fail("FlagCompute: IsDlssgEnabled is called somewhere else in it too");
    long clear = aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_CLEAR);
    if (clear < 0) return fail("FlagCompute: the stereo clear of feature 88 isn't there");
    if (clear < off) return fail("FlagCompute: the stereo clear comes before the DLSS-G check");
    plugin_log(2, "framegen: FlagCompute checked (DLSS-G check at +0x%lX, stereo clear at +0x%lX)", off, clear);
    return 1;
}

// The FSR part of FlagCompute (already checked by check_flag_compute): the IsFsrFgEnabled call between the DLSS-G check
// and the clear of 88, the only call to it there, and the clear of 89 after the one of 88. Returns IsFsrFgEnabled, or
// NULL (logged) if something doesn't match: then only FSR3 and FSR4 keep the game's rule.
static FsrFgEnabledFn check_fsr(const uint8_t *flagCompute)
{
    long dlssg = aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_DLSSG);
    long clear = aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_CLEAR);
    long off = aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_FSR);
    long clearFsr = aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_CLEAR_FSR);
    const char *why = NULL;
    uintptr_t fsr = 0;
    if (off < 0) why = "the FSR frame generation check isn't there";
    else if (off < dlssg || off > clear) why = "the FSR frame generation check isn't between the DLSS-G check and the clear";
    else if (!in_exe(fsr = call_target(flagCompute + off + FLAG_FSR_CALL), FSR_ENABLED_SIZE) ||
             aob_find_unique((const uint8_t *)fsr, FSR_ENABLED_SIZE, AOB_FSR_ENABLED) != 0)
        why = "the FSR frame generation check doesn't call IsFsrFgEnabled";
    else if (count_calls(flagCompute, FLAG_COMPUTE_SIZE, fsr) != 1) why = "IsFsrFgEnabled is called somewhere else in it too";
    else if (clearFsr < 0) why = "the stereo clear of feature 89 isn't there";
    else if (clearFsr < clear) why = "the stereo clear of feature 89 isn't after the one of 88";
    if (why) {
        fail(why);
        plugin_log(1, "framegen: FSR frame generation stays off in 3D Mode (DLSS isn't affected)");
        return NULL;
    }
    plugin_log(2, "framegen: FSR checked (IsFsrFgEnabled %p, its call at +0x%lX, stereo clear at +0x%lX)", (void *)fsr,
               off + FLAG_FSR_CALL, clearFsr);
    return (FsrFgEnabledFn)fsr;
}

// The config var is [Odyssey3D] SBSDebug.
static int check_sbs_var(const uint8_t *sbs)
{
    const char *const *names = (const char *const *)sbs;
    if (!text_is(names[VAR_NAME / 8], "SBSDebug") || !text_is(names[VAR_GROUP / 8], "Odyssey3D"))
        return fail("the SBSDebug config var isn't named [Odyssey3D] SBSDebug");
    return 1;
}

// The rest that frame generation in 3D needs: the grey, the settings lookup, FlagCompute and the SBSDebug var. Fills
// fg's lookup and SBSDebug fields when they all pass.
static int check_framegen(ResolveFn resolve, uint8_t **t)
{
    uint8_t *o3d = t[T_O3D], *setFg = t[T_SET_FG];
    uint8_t *flagCompute = t[T_FLAG_COMPUTE] = (uint8_t *)resolve(HASH_FLAG_COMPUTE);
    uint8_t *dlssg = t[T_DLSSG] = (uint8_t *)resolve(HASH_DLSSG_ENABLED);
    uint8_t *sbs = (uint8_t *)resolve(HASH_VAR_SBSDEBUG);
    plugin_log(1, "framegen: resolved FlagCompute %p, IsDlssgEnabled %p, SBSDebug var %p", (void *)flagCompute,
               (void *)dlssg, (void *)sbs);
    if (!in_exe((uintptr_t)flagCompute, FLAG_COMPUTE_SIZE) || !in_exe((uintptr_t)dlssg, 16) ||
        !in_exe((uintptr_t)sbs, VAR_VALUE + 1))
        return fail("FlagCompute, IsDlssgEnabled or the SBSDebug var didn't resolve into the game exe");
    if (aob_find_unique(o3d, O3D_SIZE, AOB_O3D_GREY) < 0) return fail("3D Mode handler: the greyed flag isn't set where expected");
    SettingsLookup l = { 0 };
    if (!check_settings_lookup(setFg, &l) || !check_flag_compute(flagCompute, dlssg) || !check_sbs_var(sbs)) return 0;
    if ((fg.tls = TlsAlloc()) == TLS_OUT_OF_INDEXES) return fail("no thread-local storage slot left");
    fg.root = (RootFn)l.root;
    fg.group = (FindFn)l.group;
    fg.var = (FindFn)l.var;
    fg.presetsName = (const uint64_t *)l.presets;
    fg.fgName = (const uint64_t *)l.name;
    fg.sbsVar = sbs;
    fg.fsrEnabled = check_fsr(flagCompute);
    return 1;
}

// Attaches hooks first to last; on a failure detaches the ones attached and returns 0.
static int attach_range(void *pluginHandle, R4eHooking *h, uint8_t **t, int first, int last)
{
    static const char *const names[T_COUNT] = { "SetFrameGeneration", "the 3D Mode handler", "IsDlssgEnabled", "FlagCompute" };
    void *const detours[T_COUNT] = { (void *)bd3d_thunk_set_fg, (void *)bd3d_thunk_o3d_changed,
                                     (void *)bd3d_thunk_dlssg_enabled, (void *)bd3d_thunk_flag_compute };
    void **const origs[T_COUNT] = { (void **)&fg.setFgOrig, (void **)&fg.o3dOrig, (void **)&fg.dlssgOrig,
                                    (void **)&fg.flagOrig };
    for (int i = first; i <= last; i++) {
        if (h->Attach(pluginHandle, t[i], detours[i], origs[i])) continue;
        plugin_log(1, "framegen: couldn't hook %s", names[i]);
        while (i-- > first) h->Detach(pluginHandle, t[i]);
        return 0;
    }
    return 1;
}

// Two groups: the handler hooks (the menu squeeze follows 3D Mode switches through them), then, if all their checks
// pass, the frame generation hooks. Either group is attached whole or not at all. Both groups are checked before
// anything is attached: the frame generation checks read SetFrameGeneration's first bytes, which the handler group's
// hook replaces with its jump.
static int attach_hooks(void *pluginHandle, R4eHooking *h, ResolveFn resolve)
{
    uint8_t *t[T_COUNT];
    int handlerOk = check_handler(resolve, t);
    int framegenOk = handlerOk && check_framegen(resolve, t);
    if (!handlerOk || !attach_range(pluginHandle, h, t, T_SET_FG, T_O3D)) {
        plugin_log(1, "framegen: nothing attached: frame generation stays off in 3D Mode, and the menu squeeze sees 3D "
                      "Mode switches only once gameplay shows them");
        return 0;
    }
    if (!framegenOk || !attach_range(pluginHandle, h, t, T_DLSSG, T_FLAG_COMPUTE)) {
        plugin_log(1, "framegen: only the 3D Mode handler is hooked (the menu squeeze follows it); frame generation "
                      "stays off in 3D Mode, as in the game");
        return 0;
    }
    fg.framegen = 1;
    plugin_log(1, "framegen: hooks attached; %s frame generation stays available in 3D Mode while [Odyssey3D] "
                  "SBSDebug is on (checked on every call: the engine ini sets it after this)",
               fg.fsrEnabled ? "DLSS and FSR" : "DLSS");
    return 1;
}

// RED4ext's address lookup, or NULL if this RED4ext has none.
static ResolveFn red4ext_resolver(void)
{
    HMODULE r4e = GetModuleHandleW(L"RED4ext.dll");
    return r4e ? (ResolveFn)(void *)GetProcAddress(r4e, "RED4ext_ResolveAddress") : NULL;
}

int framegen_load(void *pluginHandle, const R4eSdk *sdk)
{
    if (!sdk || !sdk->hooking || !sdk->hooking->Attach || !sdk->hooking->Detach)
        return fail("RED4ext didn't pass a hooking interface");
    ResolveFn resolve = red4ext_resolver();
    if (!resolve) return fail("RED4ext.dll has no RED4ext_ResolveAddress");
    return attach_hooks(pluginHandle, sdk->hooking, resolve);
}
