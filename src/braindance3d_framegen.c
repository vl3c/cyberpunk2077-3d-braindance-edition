// 3D Braindance Edition: lets the game's own DLSS, FSR (3 and 4) and XeSS frame generation run in 3D Mode, and gives the
// weapon its depth there.
//
// The game keeps frame generation off in 3D Mode in two places (static analysis of 2.5, research/native-fg):
//   - the 3D Mode settings handler sets Frame Generation to Off and greys it out whenever 3D Mode is on (it also
//     runs at start-up and on display changes, so a saved "on" doesn't survive);
//   - FlagCompute, which builds each view's render-feature bits every frame, clears feature 88 (DLSS-G), 89
//     (FSR frame generation) and 75 (XeSS frame generation) while stereo is on, so the frame graph never feeds them.
// On Samsung's monitor that is right: its weaver interlaces the eyes for the tracked eye position, and a generated
// frame would blend two woven pictures. In side by side ([Odyssey3D] SBSDebug = true) there is no weaver, and DLSS-G
// on the whole side-by-side frame works (tested through OptiScaler up to 6x), and so does the game's FSR 3.1 frame
// generation (tested in game; FSR4 and XeSS untested).
//
// Four detours through RED4ext's hooking, the addresses resolved by hash from CDPR's address library (like
// GpuApi::Present in braindance3d_menus.c):
//   - the 3D Mode handler: marks that it is running, and clears Frame Generation's greyed flag afterwards. It
//     also tells the menu squeeze which way 3D Mode was set (whether the handler asked for Off);
//   - SetFrameGeneration: skips the handler's "Off" (and so the grey) when Frame Generation is DLSS (on a PC that
//     offers DLSS frame generation), FSR3, FSR4 (the AI frame generation of AMD's RX 9000, untested), XESS (Intel
//     Arc, untested), or Off while one of those can be picked, so it keeps its setting and can be chosen in 3D Mode.
//     With a value this doesn't know (a game update's new one) selected, the game still turns it off and greys it out;
//   - FlagCompute and IsDlssgEnabled: FlagCompute calls IsDlssgEnabled only for a view that passed its other DLSS-G
//     checks, so its answer, noted per thread (FlagCompute runs for several views at once on worker threads), is
//     the game's own DLSS-G decision for that view; on stereo frames feature 88 is set again from it. FSR frame
//     generation has the same view checks, in the same place, so a view that was asked about DLSS-G is one the
//     game also asked IsFsrFgEnabled about: for it, feature 89 is set again from that function's answer (a read of
//     two flags, called by us rather than hooked). XeSS's check is there too, a read of the XeSS object's "frame
//     generation on" flag: for such a view, feature 75 is set again from that flag.
// Apart from telling the menu squeeze about 3D Mode, they change nothing unless SBSDebug is on (read on every call:
// the engine ini isn't applied yet when the plugin loads), so a real 3D monitor keeps the game's rule. Each is entered through a thunk that keeps the
// caller's registers (see REGISTER_KEEPING_THUNK). Before anything is attached, the addresses and byte patterns are
// checked (check_handler, check_framegen); if one doesn't match (a game update), that group isn't attached and the log
// says which: without the frame generation group the game keeps its rule, and without the handler group the menu squeeze
// also loses the 3D Mode switches. If only the FSR or the XeSS part doesn't match (check_fsr, check_xess), the others
// still work and that one keeps the game's rule.
// DLSS-G loads only when Frame Generation is DLSS at game start, so choosing it needs a restart, as in 2D; so do FSR and
// XeSS. With FSR at start the game presents through FidelityFX's frame generation swap chain, with XeSS through
// Intel's (libxess_fg.dll).
//
// Another detour gives the weapon its depth in side by side. The game draws the arms and the gun from a camera of
// their own (the first-person plane, with its own field of view and the Weapon Depth setting's eye separation). Its
// two eye cameras are placed left and right of its centre first, then the plane override rebuilds the plane from its
// settings, and that rebuild writes the centre's position into both eye cameras before building them: both eyes see
// the gun from the same point, so it looks flat, at the weapon convergence distance (found by the Cyberpunk VR port,
// dariulone/cyberpunk-vr-port, src/Stereo/EngineStereo.cpp). The detour keeps the eye cameras' positions across the
// rebuild and builds each moved eye again at its own place; what else the rebuild changed (field of view, near and
// far, the shift) stays. Only while SBSDebug is on, and in a group of its own: if its checks fail (check_gun), only
// the weapon keeps the game's flat look. It also steps aside while any ray tracing or path tracing is on: the game
// hands its ray-traced passes each eye's own weapon camera, but their shaders still light and shadow the weapon as if
// both eyes saw it from one place (as they do in the game), so each eye got different light and shadows on it. That
// check reads the game's settings through the lookups the frame generation and screen effects checks find, so if
// those failed, the weapon stays flat too.
//
// The handler hook also turns screen effects off when 3D Mode is switched on. On a switch in the menu the game's
// handler already turns Chromatic Aberration and Film Grain off itself (its own rule for 3D Mode); in side by side
// Depth of Field, Lens Flares, Motion Blur and Vignette ghost or blur too, so right after the handler, on the same
// switch, the hook turns those off through the game's own setters, as the menu would, and turns on the Accessibility
// settings that remove the HUD's visual effects and lens distortion (they ghost too). Nothing is greyed or locked: a
// player who turns one back on keeps it until the game next runs its own effects off (a menu switch to 3D, or another
// menu event the handler treats the same way). Only then (never at start-up), only while SBSDebug is on, and only if
// its checks passed (check_effects): otherwise only the game's two go off.
//
// License: MIT (see LICENSE).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
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
// A list setting's var: Frame Generation's (values filled in at run time), or a fixed list such as Motion Blur's.
#define SETTING_FLAGS 0x44       // flags:
#define FLAG_FROM_FILE 0x20u     //   SETTING_FILE_VALUE holds the value read from UserSettings.json (list not built yet)
#define FLAG_LIST_BUILT 0x40u    //   a run-time list is built: SETTING_VALUES is filled, SETTING_PENDING indexes it
                                 //   (fixed lists, flags 0x8F, have their values without it)
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
#define FEATURE_XESS_FG 75
#define RENDERER_XESS 0x4678     // the renderer's XeSS object
#define XESS_FG_ON 0xDD          //   its "frame generation on" flag (uint8_t), what FlagCompute reads
#define FEATURE_STEREO 98
#define FEATURE_BIT(n) (1ull << ((n) - 64)) // features 64-127 are in flags[1]
#define HASH_PLANE_OVERRIDE 4284682890u // the plane override: (float *override, camera descriptor), rebuilds the plane
#define HASH_PLANE_REBUILD 2830242058u  // the plane rebuild it calls: (camera descriptor, the plane's settings)
#define PLANE_OVERRIDE_SIZE 0x101       // its code (pdata 0x422D2C-0x422E2D)
#define PLANE_REBUILD_SIZE 0x223        // the rebuild's code (pdata 0x422AA4-0x422CC7)
// A camera descriptor (what the camera build takes).
#define CAMERA_POSITION 0x000   // int32 x, y, z in 1/131072 m
#define CAMERA_POSITION_SIZE 12
#define CAMERA_WORLD 0x280      // float x, y, z, 1: the same position, in metres
#define CAMERA_WORLD_SIZE 16
#define CAMERA_FLAGS 0x399      // uint8: bit 0 = a centre camera with two eye cameras (the pointers below)
#define CAMERA_HAS_EYES 0x01
#define CAMERA_LEFT_EYE 0x3D0   // its left eye camera's descriptor
#define CAMERA_RIGHT_EYE 0x3D8  // and its right one's
#define BOOL_SETTING_VALUE 0x58 // a bool settings var's value (what the game's getters read)
#define HASH_WEAPON_SEP 537726408u // the weapon separation getter: float (stereo settings), Weapon Depth in metres
#define WEAPON_SEP_SIZE 0x31       // its code (pdata 0x22849D0-0x2284A01)
#define SET_FG_SIZE 0xBE         // SetFrameGeneration's code (pdata 0x2075E60-0x2075F1E)
#define EVENT_NO_MENU 0          // the handler's event: uint8 [0] must be 0 and
#define EVENT_MENU 1             //   [1] 1 (a switch in the menu) for the game's own effects off
#define SET_FROM_MENU 1          // the mode the handler passes the setters (as for SetFrameGeneration(0, 1))

// Byte patterns, each found exactly once in the 2.5 exe ("??" = any byte).
// Start of the 3D Mode handler.
static const char AOB_O3D[] =
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E1 48 81 EC B8 00 00 00 4C 8B E9 E8";
// In it, the 3D-on branch: xor ecx,ecx then call SetFrameGeneration(0, 1) (the last E8).
static const char AOB_O3D_FG_OFF[] =
    "41 B4 01 E8 ?? ?? ?? ?? 0F B6 D8 E8 ?? ?? ?? ?? 41 0F B6 D4 44 0F B6 F0 33 C9 E8";
// In it, the grey: mov eax,[r15+44h]; bts eax,0Bh.
static const char AOB_O3D_GREY[] = "41 8B 47 44 0F BA E8 0B E9";
// In it, the two settings it turns off on a menu switch to 3D: var = boolVar(*group, *name) (the E8s), twice, the
// first kept at [rbp+77h] (the second is kept at [rbp+7Fh] right after the pattern).
static const char AOB_O3D_EFFECT_VARS[] =
    "48 8B 15 ?? ?? ?? ?? 48 8B F8 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 15 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? "
    "48 89 45 77 E8";
#define EFFECT_VARS_NAME1 0     // mov rdx,[CName of the first setting]
#define EFFECT_VARS_GROUP1 10   // mov rcx,[CName of its group]
#define EFFECT_VARS_CALL1 17    // call boolVar
#define EFFECT_VARS_NAME2 22
#define EFFECT_VARS_GROUP2 29
// And where it turns them off: if event[1] && !event[0]: boolSet([rbp+77h], 0, 1); boolSet([rbp+7Fh], 0, 1).
static const char AOB_O3D_EFFECTS_OFF[] =
    "41 80 7D 01 00 74 25 41 80 7D 00 00 75 1E 48 8B 4D 77 45 0F B6 C4 33 D2 E8 ?? ?? ?? ?? "
    "48 8B 4D 7F 45 0F B6 C4 33 D2 E8";
#define EFFECTS_OFF_SET1 24     // the first call to boolSet (the second is the pattern's last E8)
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
// In SetFrameGeneration, the end: index = clamp(index, the values at var+90h); listSet(var, index, mode) (the last E8).
static const char AOB_SET_FG_SET[] = "4C 8D 87 90 00 00 00 8B D3 E8 ?? ?? ?? ?? 8B D0 44 0F B6 C6 48 8B CF E8";
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
// In it, after the FSR check and behind the same view checks, the XeSS frame generation check:
// r10b = rdx->xess->fgOn (the offsets are the ones RENDERER_XESS and XESS_FG_ON name).
static const char AOB_FLAG_XESS[] = "48 8B 82 78 46 00 00 44 0F B6 90 DD 00 00 00";
// In FlagCompute, after the clear of 88, the one of feature 89: fsr && !stereo.
static const char AOB_FLAG_CLEAR_FSR[] = "45 84 DB 74 0A 45 84 E4 75 05 41 B0 01 EB 03 45 33 C0 BA 59 00 00 00 E8";
// And the one of feature 75: xess && !stereo.
static const char AOB_FLAG_CLEAR_XESS[] = "45 84 D2 74 0A 45 84 E4 75 05 41 B0 01 EB 03 45 33 C0 BA 4B 00 00 00 E8";

// Start of the plane override, through mov rbx,rdx; mov rdi,rcx (the descriptor in rbx) and its first call.
static const char AOB_PLANE_OVERRIDE[] =
    "48 89 5C 24 08 48 89 7C 24 10 55 48 8D 6C 24 A9 48 81 EC D0 00 00 00 48 8B DA 48 8B F9 E8";
// In it, the rebuild: rebuild(descriptor, &settings) (the E8).
static const char AOB_OVERRIDE_REBUILD[] = "48 8D 55 B7 48 8B CB E8";
// Start of the plane rebuild, through lea rsi,[rdx+88h].
static const char AOB_PLANE_REBUILD[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 8B 42 38 48 8D B2 88 00 00 00";
// In it, the eye cameras: the left one ([rbx+3D0h]) rebuilt by a call to itself (the E8), then the right one
// ([rbx+3D8h]) given the settings, the centre's position among them (its int x, y, z into [rbx], [rbx+8]).
static const char AOB_REBUILD_EYES[] =
    "48 8B 8B D0 03 00 00 48 8B D7 E8 ?? ?? ?? ?? 48 8B 9B D8 03 00 00 8B 47 38 F3 0F 10 0D ?? ?? ?? ?? 89 43 20 "
    "8B 47 3C 89 43 28 F2 0F 10 47 10 F2 0F 11 03 8B 47 18 89 43 08";
#define REBUILD_EYES_CALL 10
// In it, the position in metres into [rbx+280h], then the camera build of the descriptor in rbx (the last E8).
static const char AOB_REBUILD_BUILD[] =
    "0F 16 E3 0F 11 A3 80 02 00 00 0F 10 47 20 48 8B D6 0F 11 83 90 02 00 00 48 8D 8B 88 03 00 00 E8 ?? ?? ?? ?? "
    "8B 47 40 48 8B CB 89 43 40 8B 47 44 89 43 44 E8";
// In it, the loop to the eye cameras: test byte [rbx+399h],1; jnz.
static const char AOB_REBUILD_LOOP[] = "F6 83 99 03 00 00 01 0F 85";
// The weapon separation getter: under the settings lock, return the float at +0x57C (Weapon Depth, already in metres).
// The view producer reads it once a frame for the first-person plane: its eye cameras sit half of it left and right
// of the centre, and their off-axis shift uses it too.
static const char AOB_WEAPON_SEP[] =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B F9 48 83 C1 38 E8 ?? ?? ?? ?? F3 0F 10 87 7C 05 00 00 48 8D 4F 38 "
    "48 8B 5C 24 30 48 83 C4 20 5F E9";

typedef uintptr_t (*ResolveFn)(uint32_t hash);
typedef void (*O3dChangedFn)(uint8_t *event);
typedef uint8_t (*SetFgFn)(int32_t index, uint8_t mode);
typedef uint64_t *(*FlagComputeFn)(uint8_t *renderer, uint64_t *flags, void *frame, void *settings);
typedef uint8_t (*DlssgEnabledFn)(uint8_t *sl);
typedef uint8_t (*FsrFgEnabledFn)(const uint8_t *fsr);
typedef void *(*RootFn)(void);
typedef void *(*FindFn)(void *parent, uint64_t cname);
typedef int64_t (*PlaneOverrideFn)(float *override, uint8_t *camera);
typedef int64_t (*CameraBuildFn)(uint8_t *camera);
typedef float (*WeaponSepFn)(void *settings);
typedef uint8_t *(*BoolVarFn)(uint64_t group, uint64_t name);
typedef void (*BoolSetFn)(uint8_t *var, uint8_t value, uint8_t mode);
typedef void (*ListSetFn)(uint8_t *var, int32_t index, uint8_t mode);

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
    int xess;                     // the XeSS checks passed (else XeSS keeps the game's rule)
    RootFn root;
    FindFn group, var;
    const uint64_t *presetsName, *fgName; // the CNames SetFrameGeneration passes
    const uint8_t *sbsVar;
    int framegen;                 // the frame generation hooks are attached (not only the handler's)
    DWORD tls;                    // per thread: a T_* state, while FlagCompute runs on that thread
    volatile DWORD handlerThread; // the thread running the 3D Mode handler, 0 when it isn't running
    int offAsked;                 // that handler call asked for Frame Generation Off (its 3D-on branch)
    uint32_t *keptFlags;          // and it was kept: Frame Generation's flags, NULL if not kept
    RestoreCount dlssgCount, fsrCount, xessCount;
    PlaneOverrideFn planeOrig;
    CameraBuildFn cameraBuild;    // the game's camera build, which the plane rebuild calls
    RestoreCount gunCount;        // plane rebuilds whose eye cameras were put back
    WeaponSepFn sepOrig;          // the weapon separation getter (NULL if not hooked)
    volatile LONG sepZeroed;      //   it last answered 0 for ray tracing (for the log)
    volatile LONG gapTenths;      // the eye gap last logged, in 0.1 mm, plus 1 (0: none yet)
    volatile DWORD gapTick;       //   and when
    uint8_t *volatile rtVars[4];  // the ray tracing settings (RT_BOOLS, then RT Lighting), found when 3D Mode goes on
    int32_t rtLightingOff;        //   RT Lighting's index of Off
    int rtSearched;               //   looked for once (the settings exist from start-up, so a miss stays a miss)
    volatile LONG rtState;        // RT_* (what the gun hook last saw, for the log)
    BoolVarFn boolVar;            // the effects off (all set when its checks passed, else NULL): the game's lookup
    BoolSetFn boolSet;            //   of a bool setting, its bool setter, its list setter, and the CNames of the two
    ListSetFn listSet;            //   settings the game turns off itself (read when used: filled at run time)
    const uint64_t *gameOff[2];
    const uint64_t *effectsGroup; //   and the CName of their group ("/graphics/basic"), the group used for all six
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

// The engine sets the var after the plugin loads and may change it at any time, so it's read anew on every call: a
// volatile read, which the compiler can't merge with an earlier one or move across a write it can't see.
static int sbs_debug(void) { return *(const volatile uint8_t *)(fg.sbsVar + VAR_VALUE) != 0; }

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

// Whether the setting's list offers this value (DLSS: an RTX 40 or 50; FSR3: every PC; XESS: an Intel Arc in
// practice). Before the list is built
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

// The values whose frame generation runs in 3D Mode with these hooks: DLSS, FSR3 and FSR4 if the FSR checks passed,
// and XESS if the XeSS checks passed.
static int is_fsr(const char *value) { return strcmp(value, "FSR3") == 0 || strcmp(value, "FSR4") == 0; }

static int runs_in_3d(const uint8_t *var, const char *value)
{
    if (strcmp(value, "DLSS") == 0) return offers(var, "DLSS");
    if (is_fsr(value)) return fg.fsrEnabled && offers(var, value);
    if (strcmp(value, "XESS") == 0) return fg.xess && offers(var, value);
    return 0;
}

// Whether 3D Mode may keep this Frame Generation setting: one that runs in 3D, or Off while one of those can be
// chosen (FSR3 is on every list that has FSR4). A value this doesn't know (a game update's new one) stays blocked in
// stereo, so the game's rule applies to it: Off, greyed out. A value that can't be read counts as Off.
static int keep_in_3d(const uint8_t *var, const char *value, int known)
{
    if (!known || strcmp(value, "Off") == 0)
        return runs_in_3d(var, "DLSS") || runs_in_3d(var, "FSR3") || runs_in_3d(var, "XESS");
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

// The same for a hook that returns a float: xmm0 is its answer, so it isn't put back.
#define REGISTER_KEEPING_FLOAT_THUNK(thunk, impl)                                                                   \
    __asm__(".text\n.p2align 4\n.globl " #thunk "\n.def " #thunk "; .scl 2; .type 32; .endef\n"                  \
            ".seh_proc " #thunk "\n" #thunk ":\n"                                                                   \
            "push %rcx\n.seh_pushreg %rcx\npush %rdx\n.seh_pushreg %rdx\npush %r8\n.seh_pushreg %r8\n"             \
            "push %r9\n.seh_pushreg %r9\npush %r10\n.seh_pushreg %r10\npush %r11\n.seh_pushreg %r11\n"             \
            "sub $0x88, %rsp\n.seh_stackalloc 0x88\n.seh_endprologue\n" /* rsp is 16-aligned here */             \
            "movdqu %xmm1, 0x30(%rsp)\nmovdqu %xmm2, 0x40(%rsp)\n"                                                  \
            "movdqu %xmm3, 0x50(%rsp)\nmovdqu %xmm4, 0x60(%rsp)\nmovdqu %xmm5, 0x70(%rsp)\n"                      \
            "call " #impl "\n"                                                                                      \
            "movdqu 0x30(%rsp), %xmm1\nmovdqu 0x40(%rsp), %xmm2\n"                                                  \
            "movdqu 0x50(%rsp), %xmm3\nmovdqu 0x60(%rsp), %xmm4\nmovdqu 0x70(%rsp), %xmm5\n"                      \
            "add $0x88, %rsp\npop %r11\npop %r10\npop %r9\npop %r8\npop %rdx\npop %rcx\nret\n.seh_endproc\n")

#define HOOK __attribute__((used, noinline, noclone)) static // called by name from its thunk

// Clears Frame Generation's greyed flag. In the game's code only the 3D Mode handler greys it, so with a setting kept
// it's choosable again (also after an earlier run greyed it, e.g. under the 0.3.0 rule).
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

// The CName of a name: the engine's hash, 64-bit FNV-1a.
static uint64_t cname(const char *s)
{
    uint64_t h = 0xCBF29CE484222325ull;
    while (*s) h = (h ^ (uint8_t)*s++) * 0x100000001B3ull;
    return h;
}

// The screen effects that ghost or blur in side by side, all in /graphics/basic. The game itself turns the first two
// off; they're set again (to the value it just set) so the list doesn't depend on which two those are.
static const char *const EFFECTS[] = { "ChromaticAberration", "FilmGrain", "DepthOfField", "LensFlares", "Vignette" };
#define EFFECT_COUNT 5

// The name of the setting with this CName, if it's one of ours.
static const char *effect_name(uint64_t name)
{
    for (int i = 0; i < EFFECT_COUNT; i++)
        if (cname(EFFECTS[i]) == name) return EFFECTS[i];
    return name == cname("MotionBlur") ? "MotionBlur" : NULL;
}

// Where a list setting's list has this value, -1 if it doesn't (or is empty). Logs the list's state when it doesn't
// find it. FLAG_LIST_BUILT isn't needed: only lists the game fills in at run time (Frame Generation's) have it; a fixed
// list (Motion Blur's: flags 0x8F) has its values from the start.
static int32_t list_index(const uint8_t *var, const char *name)
{
    uint32_t flags = *(const uint32_t *)(var + SETTING_FLAGS);
    const uint8_t *values = *(const uint8_t *const *)(var + SETTING_VALUES);
    uint32_t count = *(const uint32_t *)(var + SETTING_VALUES + 0xC);
    char value[32];
    int read = 0;
    for (uint32_t i = 0; i < count && count <= 16; i++)
        if (read_cstring(values + (size_t)i * CSTRING_SIZE, value, sizeof(value))) {
            read++;
            if (strcmp(value, name) == 0) return (int32_t)i;
        }
    // When no value could be read as text, the first one's bytes show what the list holds instead.
    char bytes[3 * 8 + 1] = "unreadable";
    if (count && !read && readable(values, 8))
        for (int i = 0; i < 8; i++) snprintf(bytes + 3 * i, 4, i < 7 ? "%02X " : "%02X", values[i]);
    plugin_log(1, "settings: no '%s' in the list (flags 0x%X, %u values, %d read as text%s%s)", name, (unsigned)flags,
               (unsigned)count, read, count && !read ? "; the first one's bytes: " : "", count && !read ? bytes : "");
    return -1;
}

// The HUD's own effects, which ghost in opposite directions in each eye: Accessibility > Interface "Remove HUD Visual
// Effects" (its blur and glow) and "Remove HUD Lens Distortion", bools in /accessibility/interface (true = removed).
static const char *const HUD_REMOVALS[] = { "PostProcessingReduction", "LensDistortionOverride" };
#define HUD_REMOVAL_COUNT 2

// Turns the HUD's effects off: both removals on, through the game's bool setter (as the menu would).
static void hud_effects_off(void)
{
    uint64_t group = cname("/accessibility/interface");
    for (int i = 0; i < HUD_REMOVAL_COUNT; i++) {
        uint8_t *var = fg.boolVar(group, cname(HUD_REMOVALS[i]));
        if (!readable(var, SETTING_SIZE)) {
            plugin_log(1, "effects: couldn't find the %s setting, left as it is", HUD_REMOVALS[i]);
            continue;
        }
        fg.boolSet(var, 1, SET_FROM_MENU);
        plugin_log(1, "effects: %s on (the HUD's %s removed; can be turned back off)", HUD_REMOVALS[i],
                   i ? "lens distortion" : "visual effects");
    }
}

// Turns the effects off the way the game's handler turns its two off: the bools through its bool setter, Motion Blur
// (a list: Off, Low, High) to Off through its list setter. Called right after the handler, on its thread.
static void effects_off(void)
{
    uint64_t basic = *fg.effectsGroup;
    if (basic != cname("/graphics/basic"))
        plugin_log(1, "effects: the game's group for its two isn't /graphics/basic (0x%016llX), using it anyway",
                   (unsigned long long)basic);
    const char *a = effect_name(*fg.gameOff[0]), *b = effect_name(*fg.gameOff[1]);
    plugin_log(1, "effects: 3D Mode switched on in the menu: the game turned off %s and %s, turning off the rest",
               a ? a : "a setting this doesn't know", b ? b : "a setting this doesn't know");
    char done[160] = "";
    int count = 0;
    for (int i = 0; i < EFFECT_COUNT; i++) {
        uint8_t *var = fg.boolVar(basic, cname(EFFECTS[i]));
        if (!readable(var, SETTING_SIZE)) {
            plugin_log(1, "effects: couldn't find the %s setting, left as it is", EFFECTS[i]);
            continue;
        }
        fg.boolSet(var, 0, SET_FROM_MENU);
        plugin_log(2, "effects: %s off", EFFECTS[i]);
        snprintf(done + strlen(done), sizeof(done) - strlen(done), "%s%s", count++ ? ", " : "", EFFECTS[i]);
    }
    uint8_t *blur = fg.var(fg.group(fg.root(), basic), cname("MotionBlur"));
    int32_t off = readable(blur, SETTING_SIZE) ? list_index(blur, "Off") : -1;
    if (off < 0)
        plugin_log(1, "effects: couldn't find the Motion Blur setting or its Off, left as it is");
    else {
        fg.listSet(blur, off, SET_FROM_MENU);
        plugin_log(2, "effects: MotionBlur off");
        snprintf(done + strlen(done), sizeof(done) - strlen(done), "%sMotionBlur", count++ ? ", " : "");
    }
    plugin_log(1, "effects: %d of 6 off (%s; each can be turned back on)", count, count ? done : "none");
    hud_effects_off();
}

// The ray tracing settings that make the weapon's light or shadows differ between the eyes (seen in game), all in
// /graphics/raytracing: the master switch, path tracing (on its own: it can be on with the master off), Sun Shadows
// under the master, and RT Lighting (a list: Off, Medium, Ultra, Psycho), also under the master. Local Shadows and
// Reflections aren't here: the weapon keeps its depth with them (not seen to break it).
static const char *const RT_BOOLS[] = { "RayTracing", "RayTracedPathTracing", "RayTracedSunShadows" };
#define RT_MASTER 0
#define RT_PATH 1
#define RT_BOOL_COUNT 3
#define RT_LIGHTING RT_BOOL_COUNT // fg.rtVars: RT Lighting's var after the bools
_Static_assert(sizeof(fg.rtVars) / sizeof(fg.rtVars[0]) == RT_BOOL_COUNT + 1, "fg.rtVars: RT_BOOLS and RT Lighting");
#define RT_UNKNOWN 0    // fg.rtState: not looked at yet
#define RT_OFF 1        //   all off: the weapon gets its depth
#define RT_ON 2         //   one is on: the weapon stays flat
#define RT_UNREADABLE 3 //   the settings weren't found: the weapon stays flat

// Finds the ray tracing settings vars, once (they live as long as the game). Called from the 3D Mode handler hook on
// the handler's thread, right after the handler turned 3D Mode on, so before any eye camera exists: the gun hook on
// the render threads only reads what this found. Asks only for names the 2.5 game has: the game's lookups stop the
// game on a name they don't know. Does nothing if the lookups weren't found at load (their checks failed).
static void find_rt_vars(void)
{
    if (fg.rtSearched || !fg.boolVar || !fg.var) return;
    fg.rtSearched = 1; // only the handler's thread gets here
    uint64_t group = cname("/graphics/raytracing");
    uint8_t *vars[RT_BOOL_COUNT + 1];
    for (int i = 0; i < RT_BOOL_COUNT; i++)
        if (!readable(vars[i] = fg.boolVar(group, cname(RT_BOOLS[i])), BOOL_SETTING_VALUE + 1)) {
            plugin_log(1, "gun: couldn't find the %s setting: the weapon stays flat", RT_BOOLS[i]);
            return;
        }
    vars[RT_LIGHTING] = fg.var(fg.group(fg.root(), group), cname("RayTracedLighting"));
    if (!readable(vars[RT_LIGHTING], SETTING_SIZE)) {
        plugin_log(1, "gun: couldn't find the RayTracedLighting setting: the weapon stays flat");
        return;
    }
    // Off by name (the list is fixed: Off, Medium, Ultra, Psycho).
    int32_t off = list_index(vars[RT_LIGHTING], "Off");
    if (off < 0) {
        plugin_log(1, "gun: RayTracedLighting has no Off: the weapon stays flat");
        return;
    }
    fg.rtLightingOff = off;
    for (int i = 0; i < RT_BOOL_COUNT; i++) fg.rtVars[i] = vars[i];
    MemoryBarrier();
    fg.rtVars[RT_LIGHTING] = vars[RT_LIGHTING]; // last: the gun hook reads the others once it sees this
    plugin_log(2, "gun: found the ray tracing settings (RayTracedLighting's Off at %d)", (int)off);
}

static int rt_bool(int i) { return *(const volatile uint8_t *)(fg.rtVars[i] + BOOL_SETTING_VALUE) != 0; }

// Which ray tracing setting is on, NULL if none. *found is 0 if the settings weren't found.
static const char *ray_tracing_on(int *found)
{
    *found = fg.rtVars[RT_LIGHTING] != NULL;
    if (!*found) return NULL;
    if (rt_bool(RT_PATH)) return RT_BOOLS[RT_PATH];
    if (!rt_bool(RT_MASTER)) return NULL; // the rest only count under the master switch
    for (int i = RT_PATH + 1; i < RT_BOOL_COUNT; i++)
        if (rt_bool(i)) return RT_BOOLS[i];
    // RT Lighting's applied index.
    return *(const volatile int32_t *)(fg.rtVars[RT_LIGHTING] + SETTING_APPLIED) != fg.rtLightingOff
               ? "RayTracedLighting" : NULL;
}

// Whether the weapon gets its depth now (none of those ray tracing settings on). Logs each change.
static int gun_allowed(void)
{
    int found;
    const char *on = ray_tracing_on(&found);
    LONG state = !found ? RT_UNREADABLE : on ? RT_ON : RT_OFF;
    if (fg.rtState != state && InterlockedExchange(&fg.rtState, state) != state) {
        if (state == RT_UNREADABLE)
            plugin_log(1, "gun: the ray tracing settings weren't found: the weapon stays flat, as in the game");
        else if (state == RT_ON)
            plugin_log(1, "gun: ray tracing is on (%s): the weapon stays flat, as in the game (its ray-traced light "
                          "and shadows would differ between the eyes)", on);
        else
            plugin_log(1, "gun: ray tracing is off: the weapon has depth");
    }
    return state == RT_OFF;
}

HOOK void hook_o3d_changed(uint8_t *event)
{
    // event[1] is 1 when 3D Mode was switched in the menu, 0 at start-up and on a display change. Read before the
    // handler runs: it clears it on the way out.
    int ev1 = readable(event, 2) ? event[1] : -1;
    int ev0 = ev1 >= 0 ? event[0] : -1;
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
    // The game's own effects off ran under the same condition, in the branch that asked for Frame Generation Off.
    if (offAsked && ev1 == EVENT_MENU && ev0 == EVENT_NO_MENU && fg.boolVar && fg.framegen && sbs_debug())
        effects_off();
    // The gun hook's ray tracing check: its settings, found here (on this thread) before any eye camera exists.
    if (offAsked && fg.planeOrig) find_rt_vars();
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
                why = fg.fsrEnabled ? (fg.xess ? " (the game offers neither DLSS frame generation, FSR3 nor XESS)"
                                               : " (the game offers neither DLSS frame generation nor FSR3, and the "
                                                 "XeSS checks at start-up failed)")
                                    : " (the game offers no DLSS frame generation: not an RTX 40 or 50, or another mod "
                                      "such as OptiScaler holds DLSS-G; and the FSR checks at start-up failed)";
        } else if (is_fsr(value) && !fg.fsrEnabled)
            why = " (the FSR checks at start-up failed, see above)";
        else if (strcmp(value, "XESS") == 0 && !fg.xess)
            why = " (the XeSS checks at start-up failed, see above)";
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

// Feature 75: the view passed the frame generation checks (so the original read the XeSS flag too), and the XeSS
// object's "frame generation on" flag is set (only while the game runs XeSS frame generation, chosen at start).
static int should_restore_xess(uintptr_t state, uint64_t features, const uint8_t *renderer)
{
    if (!fg.xess || (state != TS_DLSSG_OFF && state != TS_DLSSG_ON) || !stereo_view_without(features, FEATURE_XESS_FG))
        return 0;
    const uint8_t *xess = renderer ? *(const uint8_t *const *)(renderer + RENDERER_XESS) : NULL;
    return xess && xess[XESS_FG_ON];
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
    if (should_restore_xess(state, bits[1], renderer)) {
        bits[1] |= FEATURE_BIT(FEATURE_XESS_FG);
        count_restored(&fg.xessCount, "XeSS");
    }
    return ret;
}

// The eye cameras of a centre camera and their places.
typedef struct {
    uint8_t *eye[2];
    uint8_t position[2][CAMERA_POSITION_SIZE];
    uint8_t world[2][CAMERA_WORLD_SIZE];
} EyePlaces;

// Notes where a centre camera's eye cameras are. 0 if it has none (a plane without stereo, or 3D Mode off).
static int keep_eye_places(const uint8_t *camera, EyePlaces *kept)
{
    if (!camera || !(camera[CAMERA_FLAGS] & CAMERA_HAS_EYES)) return 0;
    for (int e = 0; e < 2; e++) {
        uint8_t *eye = *(uint8_t *const *)(camera + (e ? CAMERA_RIGHT_EYE : CAMERA_LEFT_EYE));
        if (!eye) return 0;
        kept->eye[e] = eye;
        memcpy(kept->position[e], eye + CAMERA_POSITION, CAMERA_POSITION_SIZE);
        memcpy(kept->world[e], eye + CAMERA_WORLD, CAMERA_WORLD_SIZE);
    }
    return 1;
}

// Puts each eye camera the rebuild moved back at its place and builds it again there, the way the rebuild built it
// (the camera build, which also runs any other hook on it). An eye that didn't move (the plane already matched its
// override, so nothing was rebuilt, or the eyes sit at the centre, as while aiming) is left alone. Returns how many
// were put back.
static int restore_eye_places(const EyePlaces *kept)
{
    int moved = 0;
    for (int e = 0; e < 2; e++) {
        uint8_t *eye = kept->eye[e];
        if (memcmp(eye + CAMERA_POSITION, kept->position[e], CAMERA_POSITION_SIZE) == 0) continue;
        memcpy(eye + CAMERA_POSITION, kept->position[e], CAMERA_POSITION_SIZE);
        memcpy(eye + CAMERA_WORLD, kept->world[e], CAMERA_WORLD_SIZE);
        fg.cameraBuild(eye);
        moved++;
    }
    return moved;
}

// Counts a rebuild whose eyes were put back; logs the first one and then a count once a minute.
static void count_gun(const EyePlaces *kept)
{
    LONG64 n = InterlockedIncrement64(&fg.gunCount.restored);
    DWORD now = GetTickCount();
    if (n == 1) {
        const float *l = (const float *)kept->world[0], *r = (const float *)kept->world[1];
        plugin_log(1, "gun: first weapon view built with its own eye cameras (right eye %.1f, %.1f, %.1f mm from the "
                      "left)",
                   1000.0f * (r[0] - l[0]), 1000.0f * (r[1] - l[1]), 1000.0f * (r[2] - l[2]));
        fg.gunCount.lastSummary = now;
    } else if (now - fg.gunCount.lastSummary >= 60000) { // from several threads: a count can be off by a few, harmless
        plugin_log(1, "gun: %lld weapon views built with their own eye cameras (%lld in the last minute)", (long long)n,
                   (long long)(n - fg.gunCount.lastRestored));
        fg.gunCount.lastSummary = now;
        fg.gunCount.lastRestored = n;
    }
}

// Weapon Depth as the view producer reads it each frame. While ray tracing is on (in side by side, or the ray tracing
// settings weren't found, when the gun hook keeps the weapon flat as well) it answers 0, as if Weapon Depth were 0: the game's flat weapon then sits at screen depth in both eyes, with no shift between them
// (above 0 the flat picture is only slid apart, which looks wrong). The setting itself keeps its value, so with ray
// tracing off the weapon gets its depth from it again.
HOOK float hook_weapon_sep(void *settings)
{
    int found = 0;
    const char *on = NULL;
    LONG zero = 0;
    if (sbs_debug()) {
        on = ray_tracing_on(&found);
        zero = !found || on; // settings not found: the gun hook keeps the weapon flat, so at screen depth too
    }
    if (fg.sepZeroed != zero && InterlockedExchange(&fg.sepZeroed, zero) != zero)
        plugin_log(1, zero ? "gun: ray tracing is on (or unknown): the weapon's eye gap is 0 (as Weapon Depth 0)"
                           : "gun: the weapon's eye gap follows Weapon Depth again");
    return zero ? 0.0f : fg.sepOrig(settings);
}

// The square root of x >= 0, without the C library's (a few Newton steps from a power of two: plenty for a log).
static float root(float x)
{
    if (x <= 0.0f) return 0.0f;
    float r = 1.0f;
    while (r * r < x) r *= 2.0f;
    for (int i = 0; i < 12; i++) r = 0.5f * (r + x / r);
    return r;
}

// Logs the gap between the eye cameras the game placed when it changes by more than 0.5 mm (Weapon Depth, or the
// fade while aiming), at most once a second, so the log shows what the weapon's depth comes from. The distance, not
// the world-axis difference, so turning around doesn't change it.
static void log_gun_gap(const EyePlaces *kept)
{
    const float *l = (const float *)kept->world[0], *r = (const float *)kept->world[1];
    float d[3] = { r[0] - l[0], r[1] - l[1], r[2] - l[2] };
    LONG tenths = (LONG)(10000.0f * root(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 0.5f); // in 0.1 mm
    LONG prev = fg.gapTenths - 1; // from several threads: at worst a line too many, harmless
    DWORD now = GetTickCount();
    if (prev >= 0 && (tenths - prev <= 5 && prev - tenths <= 5)) return;
    if (prev >= 0 && now - fg.gapTick < 1000) return;
    fg.gapTenths = tenths + 1;
    fg.gapTick = now;
    plugin_log(1, "gun: the weapon's eye cameras are now %ld.%ld mm apart", (long)(tenths / 10), (long)(tenths % 10));
}

// Rebuilds the plane as the game does, then puts its eye cameras back where the game had placed them (this frame: the
// view producer assigns each plane's descriptor, eyes placed, right before calling the override). Returns what the
// original returns.
HOOK int64_t hook_plane_override(float *override, uint8_t *camera)
{
    EyePlaces kept;
    int keep = sbs_debug() && keep_eye_places(camera, &kept) && gun_allowed();
    if (keep) log_gun_gap(&kept);
    int64_t r = fg.planeOrig(override, camera);
    if (keep && restore_eye_places(&kept)) count_gun(&kept);
    return r;
}

REGISTER_KEEPING_THUNK(bd3d_thunk_o3d_changed, hook_o3d_changed);
REGISTER_KEEPING_THUNK(bd3d_thunk_set_fg, hook_set_fg);
REGISTER_KEEPING_THUNK(bd3d_thunk_dlssg_enabled, hook_dlssg_enabled);
REGISTER_KEEPING_THUNK(bd3d_thunk_flag_compute, hook_flag_compute);
void bd3d_thunk_o3d_changed(uint8_t *event);
uint8_t bd3d_thunk_set_fg(int32_t index, uint8_t mode);
uint8_t bd3d_thunk_dlssg_enabled(uint8_t *sl);
uint64_t *bd3d_thunk_flag_compute(uint8_t *renderer, uint64_t *flags, void *frame, void *settings);
REGISTER_KEEPING_THUNK(bd3d_thunk_plane_override, hook_plane_override);
int64_t bd3d_thunk_plane_override(float *override, uint8_t *camera);
REGISTER_KEEPING_FLOAT_THUNK(bd3d_thunk_weapon_sep, hook_weapon_sep);
float bd3d_thunk_weapon_sep(void *settings);

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

// The XeSS part of FlagCompute (already checked by check_flag_compute): the read of the XeSS flag between the DLSS-G
// check and the clear of 88, and the clear of 75 after the one of 88. Returns 1, or 0 (logged) if something doesn't
// match: then only XeSS keeps the game's rule.
static int check_xess(const uint8_t *flagCompute)
{
    long dlssg = aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_DLSSG);
    long clear = aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_CLEAR);
    long off = aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_XESS);
    long clearXess = aob_find_unique(flagCompute, FLAG_COMPUTE_SIZE, AOB_FLAG_CLEAR_XESS);
    const char *why = NULL;
    if (off < 0) why = "the XeSS frame generation check isn't there";
    else if (off < dlssg || off > clear) why = "the XeSS frame generation check isn't between the DLSS-G check and the clear";
    else if (clearXess < 0) why = "the stereo clear of feature 75 isn't there";
    else if (clearXess < clear) why = "the stereo clear of feature 75 isn't after the one of 88";
    if (why) {
        fail(why);
        plugin_log(1, "framegen: XeSS frame generation stays off in 3D Mode (DLSS and FSR aren't affected)");
        return 0;
    }
    plugin_log(2, "framegen: XeSS checked (its check at +0x%lX, stereo clear at +0x%lX)", off, clearXess);
    return 1;
}

// Whether the config var is [Odyssey3D] SBSDebug.
static int is_sbs_var(const uint8_t *sbs)
{
    const char *const *names = (const char *const *)sbs;
    return text_is(names[VAR_NAME / 8], "SBSDebug") && text_is(names[VAR_GROUP / 8], "Odyssey3D");
}

static int check_sbs_var(const uint8_t *sbs)
{
    return is_sbs_var(sbs) || fail("the SBSDebug config var isn't named [Odyssey3D] SBSDebug");
}

// The effects off: in the handler, its own effects off (the bool lookup, the CNames of its two settings and their
// group, the bool setter), and SetFrameGeneration's list setter. Fills fg's effects fields when they all pass, else
// logs which didn't and leaves them NULL: then only the game's two go off. Reads SetFrameGeneration's code, so it runs
// before the hooks are attached.
static void check_effects(const uint8_t *o3d, const uint8_t *setFg)
{
    long vars = aob_find_unique(o3d, O3D_SIZE, AOB_O3D_EFFECT_VARS);
    long off = aob_find_unique(o3d, O3D_SIZE, AOB_O3D_EFFECTS_OFF);
    long set = in_exe((uintptr_t)setFg, SET_FG_SIZE) ? aob_find_unique(setFg, SET_FG_SIZE, AOB_SET_FG_SET) : -1;
    const char *why = NULL;
    uintptr_t boolVar = 0, boolSet = 0, listSet = 0, slots[4] = { 0 };
    if (vars < 0) why = "the 3D Mode handler: the settings it turns off aren't looked up where expected";
    else if (off < 0) why = "the 3D Mode handler: its own effects off isn't there";
    else if (off < vars) why = "the 3D Mode handler: its effects off comes before the lookup";
    else if (set < 0) why = "SetFrameGeneration: the list setter call isn't there";
    else {
        const uint8_t *v = o3d + vars, *o = o3d + off;
        boolVar = call_target(v + EFFECT_VARS_CALL1);
        boolSet = call_target(o + EFFECTS_OFF_SET1);
        listSet = call_target(setFg + set + aob_len(AOB_SET_FG_SET) - 1);
        slots[0] = rip_operand(v + EFFECT_VARS_NAME1);
        slots[1] = rip_operand(v + EFFECT_VARS_GROUP1);
        slots[2] = rip_operand(v + EFFECT_VARS_NAME2);
        slots[3] = rip_operand(v + EFFECT_VARS_GROUP2);
        if (call_target(v + aob_len(AOB_O3D_EFFECT_VARS) - 1) != boolVar)
            why = "the 3D Mode handler: its two settings aren't looked up the same way";
        else if (call_target(o + aob_len(AOB_O3D_EFFECTS_OFF) - 1) != boolSet)
            why = "the 3D Mode handler: its two settings aren't turned off the same way";
        else if (!in_exe(boolVar, 16) || !in_exe(boolSet, 16) || !in_exe(listSet, 16))
            why = "the settings lookup or setters point outside the exe";
        else
            for (int i = 0; i < 4 && !why; i++)
                if (!in_exe(slots[i], 8)) why = "the 3D Mode handler: its settings' names point outside the exe";
    }
    if (why) {
        plugin_log(1, "effects: %s (game update?)", why);
        plugin_log(1, "effects: switching 3D Mode on turns off only what the game turns off (Chromatic Aberration and "
                      "Film Grain)");
        return;
    }
    fg.boolVar = (BoolVarFn)boolVar;
    fg.boolSet = (BoolSetFn)boolSet;
    fg.listSet = (ListSetFn)listSet;
    fg.gameOff[0] = (const uint64_t *)slots[0];
    fg.gameOff[1] = (const uint64_t *)slots[2];
    fg.effectsGroup = (const uint64_t *)slots[1];
    plugin_log(2, "effects: checked (the handler's lookup at +0x%lX, its effects off at +0x%lX)", vars, off);
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
    fg.xess = check_xess(flagCompute);
    check_effects(o3d, setFg);
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
               fg.fsrEnabled ? (fg.xess ? "DLSS, FSR and XeSS" : "DLSS and FSR") : (fg.xess ? "DLSS and XeSS" : "DLSS"));
    return 1;
}

// The plane override and its rebuild: the override's start and its rebuild call going to the rebuild (whose hash
// resolves to the same place, called once); in the rebuild, the eye cameras at +0x3D0 / +0x3D8 given the centre's
// position at +0x0 and +0x280, the camera build after it (whose address the hook calls), and the loop to the eye
// cameras on flag bit 0 of +0x399. Also the SBSDebug var. Returns the plane override, or NULL (logged): then the
// weapon keeps the game's flat look.
static uint8_t *check_gun(ResolveFn resolve)
{
    uint8_t *override = (uint8_t *)resolve(HASH_PLANE_OVERRIDE);
    uint8_t *rebuild = (uint8_t *)resolve(HASH_PLANE_REBUILD);
    uint8_t *sbs = (uint8_t *)resolve(HASH_VAR_SBSDEBUG);
    plugin_log(1, "gun: resolved the plane override %p, the plane rebuild %p", (void *)override, (void *)rebuild);
    const char *why = NULL;
    long call = -1, eyes = -1, build = -1;
    uintptr_t buildFn = 0;
    if (!in_exe((uintptr_t)override, PLANE_OVERRIDE_SIZE) || !in_exe((uintptr_t)rebuild, PLANE_REBUILD_SIZE) ||
        !in_exe((uintptr_t)sbs, VAR_VALUE + 1))
        why = "the plane override, the plane rebuild or the SBSDebug var didn't resolve into the game exe";
    else if (aob_find_unique(override, PLANE_OVERRIDE_SIZE, AOB_PLANE_OVERRIDE) != 0)
        why = "the plane override: start doesn't match";
    else if ((call = aob_find_unique(override, PLANE_OVERRIDE_SIZE, AOB_OVERRIDE_REBUILD)) < 0)
        why = "the plane override: the rebuild call isn't there";
    else if (call_target(override + call + aob_len(AOB_OVERRIDE_REBUILD) - 1) != (uintptr_t)rebuild)
        why = "the plane override: the rebuild call doesn't go to the plane rebuild";
    else if (count_calls(override, PLANE_OVERRIDE_SIZE, (uintptr_t)rebuild) != 1)
        why = "the plane override calls the plane rebuild somewhere else too";
    else if (aob_find_unique(rebuild, PLANE_REBUILD_SIZE, AOB_PLANE_REBUILD) != 0)
        why = "the plane rebuild: start doesn't match";
    else if ((eyes = aob_find_unique(rebuild, PLANE_REBUILD_SIZE, AOB_REBUILD_EYES)) < 0)
        why = "the plane rebuild: the eye cameras aren't given the position where expected";
    else if (call_target(rebuild + eyes + REBUILD_EYES_CALL) != (uintptr_t)rebuild)
        why = "the plane rebuild: the left eye camera isn't rebuilt by the rebuild";
    else if ((build = aob_find_unique(rebuild, PLANE_REBUILD_SIZE, AOB_REBUILD_BUILD)) < 0)
        why = "the plane rebuild: the camera build after the position isn't there";
    else if (!in_exe(buildFn = call_target(rebuild + build + aob_len(AOB_REBUILD_BUILD) - 1), 16))
        why = "the plane rebuild: the camera build call points outside the exe";
    else if (count_calls(rebuild, PLANE_REBUILD_SIZE, buildFn) != 1)
        why = "the plane rebuild calls the camera build somewhere else too";
    else if (aob_find_unique(rebuild, PLANE_REBUILD_SIZE, AOB_REBUILD_LOOP) < 0)
        why = "the plane rebuild: the loop to the eye cameras isn't there";
    else if (!is_sbs_var(sbs)) why = "the SBSDebug config var isn't named [Odyssey3D] SBSDebug";
    if (why) {
        plugin_log(1, "gun: %s (game update?)", why);
        plugin_log(1, "gun: not hooked: in 3D the weapon stays flat, as in the game");
        return NULL;
    }
    fg.sbsVar = sbs;
    fg.cameraBuild = (CameraBuildFn)buildFn;
    plugin_log(2, "gun: plane override checked (rebuild call at +0x%lX, camera build %p)",
               call + (long)aob_len(AOB_OVERRIDE_REBUILD) - 1, (void *)buildFn);
    return override;
}

// The weapon separation getter: checked, then hooked. Part of the gun group (only with the plane override hooked); if
// it fails, the weapon under ray tracing keeps the game's Weapon Depth shift.
static int attach_weapon_sep(void *pluginHandle, R4eHooking *h, ResolveFn resolve)
{
    uint8_t *sep = (uint8_t *)resolve(HASH_WEAPON_SEP);
    const char *why = NULL;
    if (!in_exe((uintptr_t)sep, WEAPON_SEP_SIZE)) why = "the weapon separation getter didn't resolve into the game exe";
    else if (aob_find_unique(sep, WEAPON_SEP_SIZE, AOB_WEAPON_SEP) != 0) why = "the weapon separation getter doesn't match";
    else if (!h->Attach(pluginHandle, sep, (void *)bd3d_thunk_weapon_sep, (void **)&fg.sepOrig)) {
        fg.sepOrig = NULL;
        why = "couldn't hook the weapon separation getter";
    }
    if (why) {
        plugin_log(1, "gun: %s (game update?): with ray tracing on the flat weapon keeps Weapon Depth's shift", why);
        return 0;
    }
    plugin_log(1, "gun: weapon separation hooked %p: with ray tracing on the weapon's eye gap is 0", (void *)sep);
    return 1;
}

// The gun group: checked, then attached on its own. Returns 1 when attached.
static int attach_gun(void *pluginHandle, R4eHooking *h, ResolveFn resolve)
{
    uint8_t *override = check_gun(resolve);
    if (!override) return 0;
    if (!h->Attach(pluginHandle, override, (void *)bd3d_thunk_plane_override, (void **)&fg.planeOrig)) {
        plugin_log(1, "gun: couldn't hook the plane override: in 3D the weapon stays flat, as in the game");
        return 0;
    }
    if (!fg.boolVar || !fg.var) {
        plugin_log(1, "gun: hook attached, but the ray tracing settings can't be read (the frame generation or screen "
                      "effects checks failed): the weapon stays flat, as in the game");
        return 1;
    }
    plugin_log(1, "gun: hook attached; in side by side the weapon is seen from both eyes with ray tracing off (while "
                  "[Odyssey3D] SBSDebug is on; Weapon Depth sets how far apart)");
    attach_weapon_sep(pluginHandle, h, resolve);
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
    int attached = attach_hooks(pluginHandle, sdk->hooking, resolve);
    attach_gun(pluginHandle, sdk->hooking, resolve);
    return attached;
}
