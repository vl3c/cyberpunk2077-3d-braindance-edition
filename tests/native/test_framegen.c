// White-box tests for the frame generation hooks: this file #includes src/braindance3d_framegen.c so it can run
// the byte-pattern checks against a fake copy of the game's code and drive the hooks with fake originals.
// Whether frame generation really runs in 3D is only tested in game: DLSS and FSR3 are; FSR4 and XeSS are untested.
//
// Run: test_framegen.exe [--list] [name filters...]

#include <stdio.h>
#include <stdarg.h>

#include "../../src/braindance3d_framegen.c"

#define TT_IMPLEMENTATION
#include "tinytest.h"
#include "testutil.h"

// ===================================================================================== fakes

static char ft_log[8192];
static int f_noted3d = -1; // what the menu squeeze was last told

void menus_note_3d_mode(int on) { f_noted3d = on; }

void plugin_log(int level, const char *fmt, ...)
{
    size_t n = strlen(ft_log);
    if (n + 2 >= sizeof(ft_log)) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ft_log + n, sizeof(ft_log) - n - 1, fmt, ap);
    va_end(ap);
    strcat(ft_log, "\n");
}

// A fake copy of the game's code and data, inside this exe's image (like the real targets are inside the game's).
static uint8_t f_o3d[O3D_SIZE], f_setFg[SET_FG_SIZE], f_other[16], f_flag[FLAG_COMPUTE_SIZE], f_dlssg[16], f_lookup[3][16];
static uint8_t f_fsrCode[FSR_ENABLED_SIZE]; // IsFsrFgEnabled's bytes (never run: the hook tests call fake_fsr_enabled)
static uint64_t f_cnames[2];
static uint8_t f_override[PLANE_OVERRIDE_SIZE], f_rebuild[PLANE_REBUILD_SIZE], f_buildCode[16], f_sep[WEAPON_SEP_SIZE];
static uint8_t f_boolVarCode[16], f_boolSetCode[16], f_listSetCode[16];
static uint64_t f_effectSlots[4]; // the handler's CNames: its two settings and their group, twice
static const char *f_sbsVar[8];
static int f_attachFails; // which Attach call fails (1-based), 0 = none
static int f_attached, f_detached;

// Writes pattern at p ("??" bytes become 0) and returns the byte after it.
static uint8_t *put(uint8_t *p, const char *pattern)
{
    uint8_t bytes[128], mask[128];
    int n = aob_parse(pattern, bytes, mask, (int)sizeof(bytes));
    memcpy(p, bytes, n);
    return p + n;
}

static void put_rel32(uint8_t *insnEnd, const void *target) { ((int32_t *)insnEnd)[-1] = (int32_t)((const uint8_t *)target - insnEnd); }

// Lays the fake functions out the way the 2.5 exe has them.
static void fake_game(void)
{
    memset(f_o3d, 0xCC, sizeof(f_o3d));
    memset(f_flag, 0xCC, sizeof(f_flag));
    put(f_o3d, AOB_O3D);
    uint8_t *end = put(f_o3d + 0x188, AOB_O3D_FG_OFF);
    memset(end, 0, 4);
    put_rel32(end + 4, f_setFg); // the trailing E8's rel32
    put(f_o3d + 0x2FE, AOB_O3D_GREY);
    uint8_t *v = f_o3d + 0xDF;
    put(v, AOB_O3D_EFFECT_VARS);
    put_rel32(v + EFFECT_VARS_NAME1 + 7, &f_effectSlots[0]);
    put_rel32(v + EFFECT_VARS_GROUP1 + 7, &f_effectSlots[1]);
    put_rel32(v + EFFECT_VARS_CALL1 + 5, f_boolVarCode);
    put_rel32(v + EFFECT_VARS_NAME2 + 7, &f_effectSlots[2]);
    put_rel32(v + EFFECT_VARS_GROUP2 + 7, &f_effectSlots[3]);
    put_rel32(v + aob_len(AOB_O3D_EFFECT_VARS) + 4, f_boolVarCode);
    uint8_t *o = f_o3d + 0x2D2;
    put(o, AOB_O3D_EFFECTS_OFF);
    put_rel32(o + EFFECTS_OFF_SET1 + 5, f_boolSetCode);
    put_rel32(o + aob_len(AOB_O3D_EFFECTS_OFF) + 4, f_boolSetCode);

    put(f_setFg, AOB_SET_FG);
    put_rel32(f_setFg + SET_FG_ROOT + 5, f_lookup[0]);
    put_rel32(f_setFg + SET_FG_PRESETS + 7, &f_cnames[0]);
    put_rel32(f_setFg + SET_FG_GROUP + 5, f_lookup[1]);
    put_rel32(f_setFg + SET_FG_NAME + 7, &f_cnames[1]);
    put_rel32(f_setFg + SET_FG_VAR + 5, f_lookup[2]);
    put_rel32(put(f_setFg + 0x90, AOB_SET_FG_SET) + 4, f_listSetCode);

    put(f_flag, AOB_FLAG_COMPUTE);
    put(f_flag + 0x5BC, AOB_FLAG_DLSSG);
    put_rel32(f_flag + 0x5BC + FLAG_DLSSG_CALL + 5, f_dlssg);
    put(f_flag + 0x660, AOB_FLAG_FSR);
    put_rel32(f_flag + 0x660 + FLAG_FSR_CALL + 5, f_fsrCode);
    put(f_flag + 0x6A0, AOB_FLAG_XESS);
    put(f_flag + 0x6F0, AOB_FLAG_CLEAR);
    put(f_flag + 0x707, AOB_FLAG_CLEAR_FSR);
    put(f_flag + 0x730, AOB_FLAG_CLEAR_XESS);
    put(f_fsrCode, AOB_FSR_ENABLED);

    memset(f_override, 0xCC, sizeof(f_override));
    memset(f_rebuild, 0xCC, sizeof(f_rebuild));
    put(f_override, AOB_PLANE_OVERRIDE);
    put_rel32(put(f_override + 0xCB, AOB_OVERRIDE_REBUILD) + 4, f_rebuild);
    put(f_rebuild, AOB_PLANE_REBUILD);
    put(f_rebuild + 0xE2, AOB_REBUILD_EYES);
    put_rel32(f_rebuild + 0xE2 + REBUILD_EYES_CALL + 5, f_rebuild); // the left eye: the rebuild calls itself
    put_rel32(put(f_rebuild + 0x19E, AOB_REBUILD_BUILD) + 4, f_buildCode);
    put(f_rebuild + 0x201, AOB_REBUILD_LOOP);
    memset(f_sep, 0xCC, sizeof(f_sep));
    put(f_sep, AOB_WEAPON_SEP);

    f_sbsVar[VAR_NAME / 8] = "SBSDebug";
    f_sbsVar[VAR_GROUP / 8] = "Odyssey3D";
    ((uint8_t *)f_sbsVar)[VAR_VALUE] = 1;
}

static uintptr_t fake_resolve(uint32_t hash)
{
    switch (hash) {
    case HASH_O3D_CHANGED: return (uintptr_t)f_o3d;
    case HASH_SET_FG: return (uintptr_t)f_setFg;
    case HASH_FLAG_COMPUTE: return (uintptr_t)f_flag;
    case HASH_DLSSG_ENABLED: return (uintptr_t)f_dlssg;
    case HASH_VAR_SBSDEBUG: return (uintptr_t)f_sbsVar;
    case HASH_PLANE_OVERRIDE: return (uintptr_t)f_override;
    case HASH_PLANE_REBUILD: return (uintptr_t)f_rebuild;
    case HASH_WEAPON_SEP: return (uintptr_t)f_sep;
    }
    return 0;
}

static uintptr_t resolve_nothing(uint32_t hash) { return 0; }

// Like RED4ext's: the target starts with a jump to the detour from then on (here only the jump's opcode and a zero
// offset, enough to change the bytes the checks read).
static bool fake_attach(void *handle, void *target, void *detour, void **original)
{
    if (++f_attached == f_attachFails) return false;
    memcpy(target, "\xE9\0\0\0\0", 5);
    *original = target;
    return true;
}

static bool fake_detach(void *handle, void *target)
{
    f_detached++;
    return true;
}

static R4eHooking f_hooking = { fake_attach, fake_detach };

// Fake originals and game functions for the hook tests.
static int f_setFgCalls, f_setFgIndex, f_o3dSetsFgOff, f_o3dGreys, f_eligible, f_dlssgOn, f_dlssgCalls;
static uint8_t f_var[SETTING_SIZE];
static uint8_t f_values[5][CSTRING_SIZE]; // the setting's list, as the game builds it

// Puts a short CString (inline) or a long one (on the heap) at p.
static void put_cstring(uint8_t *p, const char *text)
{
    static char heap[5][64];
    static int next;
    memset(p, 0, CSTRING_SIZE);
    uint32_t len = (uint32_t)strlen(text);
    if (len < 16) {
        memcpy(p, text, len);
        *(uint32_t *)(p + CSTRING_LEN_FLAGS) = len;
    } else {
        char *h = heap[next++ % 5];
        strcpy(h, text);
        *(char **)p = h;
        *(uint32_t *)(p + CSTRING_LEN_FLAGS) = len | CSTRING_HEAP;
    }
}

// The menu shows `value` from a built list (Off, DLSS, FSR3, XESS on this "PC"), or, with list < 0, the setting still
// holds the value read from the file.
static void set_fg_value(const char *value, int list)
{
    static const char *const names[] = { "Off", "DLSS", "FSR3", "XESS" };
    uint32_t *flags = (uint32_t *)(f_var + SETTING_FLAGS);
    *flags &= ~(FLAG_FROM_FILE | FLAG_LIST_BUILT);
    if (list < 0) {
        *flags |= FLAG_FROM_FILE;
        put_cstring(f_var + SETTING_FILE_VALUE, value);
        return;
    }
    *flags |= FLAG_LIST_BUILT;
    int index = -1;
    for (int i = 0; i < 4; i++) {
        put_cstring(f_values[i], names[i]);
        if (!strcmp(names[i], value)) index = i;
    }
    *(uint8_t **)(f_var + SETTING_VALUES) = &f_values[0][0];
    *(uint32_t *)(f_var + SETTING_VALUES + 0xC) = 4;
    *(int32_t *)(f_var + SETTING_PENDING) = index;
    *(int32_t *)(f_var + SETTING_APPLIED) = index;
}

static uint8_t orig_set_fg(int32_t index, uint8_t mode)
{
    f_setFgCalls++;
    f_setFgIndex = index;
    return 1;
}

// Does what the game's handler does with 3D Mode on: Frame Generation Off, then the greyed flag.
// Clears event[1] on the way out, as the game's does (the thunk test passes a junk event).
static void orig_o3d(uint8_t *event)
{
    if (f_o3dSetsFgOff) bd3d_thunk_set_fg(0, 1); // the game calls the patched function, so through the thunk
    if (f_o3dGreys) *(uint32_t *)(f_var + SETTING_FLAGS) |= FLAG_GREYED;
    if (readable(event, 2)) event[1] = 0;
}

// The screen effect settings: five bools and Motion Blur (a list: Off, Low, High), and what was set.
static uint8_t f_bools[EFFECT_COUNT][SETTING_SIZE], f_blur[SETTING_SIZE], f_blurValues[3][CSTRING_SIZE];
static int f_boolSets, f_listSets, f_missingBool = -1;
static uint8_t *f_boolSetVar[8], f_boolSetValue[8], f_boolSetMode[8];
static int32_t f_listSetIndex;
static uint8_t f_listSetMode;
static uint8_t *f_listSetVar;

// The ray tracing settings: RT_BOOLS, then RT Lighting (a list), and how often they were looked up.
static uint8_t f_rtBools[RT_BOOL_COUNT][SETTING_SIZE], f_rtLighting[SETTING_SIZE], f_rtGroup[8];
static int f_rtLookups;
static uint8_t f_hud[HUD_REMOVAL_COUNT][SETTING_SIZE]; // the HUD removals in /accessibility/interface

static uint8_t *fake_bool_var(uint64_t group, uint64_t name)
{
    if (group == cname("/graphics/raytracing")) {
        f_rtLookups++;
        for (int i = 0; i < RT_BOOL_COUNT; i++)
            if (name == cname(RT_BOOLS[i])) return f_rtBools[i];
        CHECK_MSG(0, "looked up a ray tracing setting the 2.5 game doesn't have (the game would stop)");
        return NULL;
    }
    if (group == cname("/accessibility/interface")) {
        for (int i = 0; i < HUD_REMOVAL_COUNT; i++)
            if (name == cname(HUD_REMOVALS[i])) return f_hud[i];
        CHECK_MSG(0, "looked up an interface setting the 2.5 game doesn't have (the game would stop)");
        return NULL;
    }
    if (group != cname("/graphics/basic")) return NULL;
    for (int i = 0; i < EFFECT_COUNT; i++)
        if (name == cname(EFFECTS[i])) return i == f_missingBool ? NULL : f_bools[i];
    return NULL;
}

static void fake_bool_set(uint8_t *var, uint8_t value, uint8_t mode)
{
    if (f_boolSets < 8) {
        f_boolSetVar[f_boolSets] = var;
        f_boolSetValue[f_boolSets] = value;
        f_boolSetMode[f_boolSets] = mode;
    }
    f_boolSets++;
}

static void fake_list_set(uint8_t *var, int32_t index, uint8_t mode)
{
    f_listSets++;
    f_listSetVar = var;
    f_listSetIndex = index;
    f_listSetMode = mode;
}

static void fake_effects(void)
{
    static const char *const names[] = { "Off", "Low", "High" };
    memset(f_bools, 0, sizeof(f_bools));
    memset(f_blur, 0, sizeof(f_blur));
    for (int i = 0; i < 3; i++) put_cstring(f_blurValues[i], names[i]);
    *(uint32_t *)(f_blur + SETTING_FLAGS) = 0x8F; // as in the game: a fixed list, without FLAG_LIST_BUILT
    *(uint8_t **)(f_blur + SETTING_VALUES) = &f_blurValues[0][0];
    *(uint32_t *)(f_blur + SETTING_VALUES + 0xC) = 3;
    f_effectSlots[0] = cname("ChromaticAberration");
    f_effectSlots[1] = f_effectSlots[3] = cname("/graphics/basic");
    f_effectSlots[2] = cname("FilmGrain");
    f_boolSets = f_listSets = 0;
    f_missingBool = -1;
    f_listSetVar = NULL;
    f_listSetIndex = -1;
    fg.boolVar = fake_bool_var;
    fg.boolSet = fake_bool_set;
    fg.listSet = fake_list_set;
    fg.gameOff[0] = &f_effectSlots[0];
    fg.gameOff[1] = &f_effectSlots[2];
    fg.effectsGroup = &f_effectSlots[1];
    memset(f_rtBools, 0, sizeof(f_rtBools)); // all ray tracing off
    static uint8_t lightingValues[4][CSTRING_SIZE];
    static const char *const lighting[] = { "Off", "Medium", "Ultra", "Psycho" };
    memset(f_rtLighting, 0, sizeof(f_rtLighting));
    for (int i = 0; i < 4; i++) put_cstring(lightingValues[i], lighting[i]);
    *(uint32_t *)(f_rtLighting + SETTING_FLAGS) = 0x8F; // a fixed list, as Motion Blur's
    *(uint8_t **)(f_rtLighting + SETTING_VALUES) = &lightingValues[0][0];
    *(uint32_t *)(f_rtLighting + SETTING_VALUES + 0xC) = 4;
    f_rtLookups = 0;
    memset((void *)fg.rtVars, 0, sizeof(fg.rtVars));
    fg.rtSearched = 0;
    fg.rtState = RT_UNKNOWN;
}

static uint8_t f_basicGroup[8];
static void *fake_root(void) { return f_lookup; }
static void *fake_find(void *parent, uint64_t name)
{
    if (name == cname("/graphics/basic")) return f_basicGroup;
    if (name == cname("/graphics/raytracing")) return f_rtGroup;
    if (name == cname("RayTracedLighting")) return parent == f_rtGroup ? f_rtLighting : NULL;
    if (name == cname("MotionBlur")) return parent == f_basicGroup ? f_blur : NULL;
    return name == f_cnames[1] ? (void *)f_var : (void *)f_lookup;
}
static void *fake_find_nothing(void *parent, uint64_t cname) { return NULL; }

// Like the game's IsFsrFgEnabled: fsr[0x59] && !fsr[0x5A]. f_renderer's FSR object is f_fsr, its XeSS object f_xess.
static uint8_t f_fsr[0x100], f_xess[0x100], f_renderer[RENDERER_XESS + 8];
static uint8_t fake_fsr_enabled(const uint8_t *fsr) { return fsr[0x59] && !fsr[0x5A]; }

static uint8_t fake_dlssg(uint8_t *sl)
{
    f_dlssgCalls++;
    return (uint8_t)f_dlssgOn;
}

// Like the game's: asks IsDlssgEnabled (through the hook, as it would be patched) only for an eligible view.
static uint64_t f_flagsAfterOrig[2];
static DWORD f_pauseThread;           // this thread's FlagCompute pauses after asking, until f_resume is set
static HANDLE f_asked, f_resume;
static uint64_t *orig_flag_compute(uint8_t *renderer, uint64_t *flags, void *frame, void *settings)
{
    if (f_eligible) hook_dlssg_enabled(NULL);
    if (f_pauseThread == GetCurrentThreadId()) {
        SetEvent(f_asked);
        WaitForSingleObject(f_resume, 10000);
    }
    flags[0] = f_flagsAfterOrig[0];
    flags[1] = f_flagsAfterOrig[1];
    return flags; // the game's caller reads the bits through this
}

// A centre camera with two eye cameras, a plane override that moves them to the centre (like the game's rebuild), and
// a camera build that notes what it was given.
static _Alignas(16) uint8_t f_centre[0x400], f_eyes[2][0x400];
static int f_overrideCalls, f_rebuildMoves, f_builds;
static uint8_t *f_built[4];
static int32_t f_builtAt[4][3]; // each built eye's position when it was built
static float *f_overrideSeen;
static uint8_t *f_cameraSeen;

static void place(uint8_t *camera, int32_t x, int32_t y, int32_t z)
{
    int32_t *p = (int32_t *)(camera + CAMERA_POSITION);
    float *w = (float *)(camera + CAMERA_WORLD);
    p[0] = x, p[1] = y, p[2] = z;
    w[0] = x / 131072.0f, w[1] = y / 131072.0f, w[2] = z / 131072.0f, w[3] = 1.0f;
}

static void fake_cameras(void)
{
    memset(f_centre, 0, sizeof(f_centre));
    memset(f_eyes, 0, sizeof(f_eyes));
    f_centre[CAMERA_FLAGS] = CAMERA_HAS_EYES;
    *(uint8_t **)(f_centre + CAMERA_LEFT_EYE) = f_eyes[0];
    *(uint8_t **)(f_centre + CAMERA_RIGHT_EYE) = f_eyes[1];
    f_eyes[0][CAMERA_FLAGS] = f_eyes[1][CAMERA_FLAGS] = 0x02; // eye cameras
    place(f_centre, 131072, 262144, 65536);
    place(f_eyes[0], 131072 - 4194, 262144, 65536); // 32 mm left and right of the centre
    place(f_eyes[1], 131072 + 4194, 262144, 65536);
}

static int64_t orig_plane_override(float *override, uint8_t *camera)
{
    f_overrideCalls++;
    f_overrideSeen = override;
    f_cameraSeen = camera;
    if (f_rebuildMoves && camera && (camera[CAMERA_FLAGS] & CAMERA_HAS_EYES))
        for (int e = 0; e < 2; e++) {
            uint8_t *eye = *(uint8_t **)(camera + (e ? CAMERA_RIGHT_EYE : CAMERA_LEFT_EYE));
            memcpy(eye + CAMERA_POSITION, camera + CAMERA_POSITION, CAMERA_POSITION_SIZE);
            memcpy(eye + CAMERA_WORLD, camera + CAMERA_WORLD, CAMERA_WORLD_SIZE);
        }
    return 0x5678;
}

static int64_t fake_camera_build(uint8_t *camera)
{
    if (f_builds < 4) {
        f_built[f_builds] = camera;
        memcpy(f_builtAt[f_builds], camera + CAMERA_POSITION, CAMERA_POSITION_SIZE);
    }
    f_builds++;
    return 0;
}

static void ft_begin(void)
{
    static DWORD tls = TLS_OUT_OF_INDEXES;
    if (tls == TLS_OUT_OF_INDEXES) tls = TlsAlloc();
    memset(&fg, 0, sizeof(fg));
    fg.tls = tls;
    fg.framegen = 1;
    f_noted3d = -1;
    ft_log[0] = 0;
    fake_game();
    f_attachFails = f_attached = f_detached = 0;
    f_setFgCalls = f_setFgIndex = 0;
    f_o3dSetsFgOff = f_o3dGreys = 1;
    f_eligible = f_dlssgOn = 1;
    f_dlssgCalls = 0;
    memset(f_var, 0, sizeof(f_var));
    set_fg_value("DLSS", 1);
    f_cnames[0] = 0x1AF15AB3D7A612E0ull;
    f_cnames[1] = 0x1ACE52F474ADA088ull;
    fg.setFgOrig = orig_set_fg;
    fg.o3dOrig = orig_o3d;
    fg.flagOrig = orig_flag_compute;
    fg.dlssgOrig = fake_dlssg;
    fg.fsrEnabled = fake_fsr_enabled;
    memset(f_fsr, 0, sizeof(f_fsr));
    f_fsr[0x59] = 1; // FSR frame generation on
    *(uint8_t **)(f_renderer + RENDERER_FSR) = f_fsr;
    fg.xess = 1;
    memset(f_xess, 0, sizeof(f_xess)); // XeSS frame generation off (the tests that want it on set XESS_FG_ON)
    *(uint8_t **)(f_renderer + RENDERER_XESS) = f_xess;
    fg.root = fake_root;
    fg.group = fake_find;
    fg.var = fake_find;
    fg.presetsName = &f_cnames[0];
    fg.fgName = &f_cnames[1];
    fg.sbsVar = (const uint8_t *)f_sbsVar;
    f_flagsAfterOrig[0] = 0;
    f_flagsAfterOrig[1] = FEATURE_BIT(FEATURE_STEREO);
    fake_cameras();
    fg.planeOrig = orig_plane_override;
    fg.cameraBuild = fake_camera_build;
    f_overrideCalls = f_builds = 0;
    f_rebuildMoves = 1;
    f_overrideSeen = NULL;
    f_cameraSeen = NULL;
    fake_effects();
    find_rt_vars(); // as the handler hook does when 3D Mode goes on
    f_rtLookups = 0;
}

static void set_sbs_debug(int on) { ((uint8_t *)f_sbsVar)[VAR_VALUE] = (uint8_t)on; }

// The 3D Mode handler hook, called with this event (bytes [0] and [1]).
static void menu_switch(uint8_t ev0, uint8_t ev1)
{
    uint8_t event[8] = { ev0, ev1 };
    hook_o3d_changed(event);
}

// ===================================================================================== byte patterns

TEST(aob_finds_the_only_match_with_wildcards)
{
    const uint8_t code[] = { 0x90, 0x48, 0x8B, 0x05, 0x11, 0x22, 0xC3, 0x90 };
    CHECK_EQ_INT(aob_find_unique(code, sizeof(code), "48 8B ?? 11 22 C3"), 1);
    CHECK_EQ_INT(aob_find_unique(code, sizeof(code), "48 8B 05"), 1);
    CHECK_EQ_INT(aob_find_unique(code, sizeof(code), "C3 90"), 6); // right at the end
    CHECK_EQ_INT(aob_find_unique(code, sizeof(code), "90"), -1);    // twice
    CHECK_EQ_INT(aob_find_unique(code, sizeof(code), "48 8B 06"), -1);
    CHECK_EQ_INT(aob_find_unique(code, sizeof(code), "C3 90 90"), -1); // would run past the end
}

TEST(aob_rejects_malformed_patterns)
{
    const uint8_t code[] = { 0x48, 0x8B, 0x05 };
    CHECK_EQ_INT(aob_find_unique(code, sizeof(code), ""), -1);
    CHECK_EQ_INT(aob_find_unique(code, sizeof(code), "48 8"), -1);
    CHECK_EQ_INT(aob_find_unique(code, sizeof(code), "488B"), -1);
    CHECK_EQ_INT(aob_find_unique(code, sizeof(code), "48 XY"), -1);
    CHECK_EQ_INT(aob_len(AOB_FLAG_CLEAR), 19);
    CHECK_EQ_INT(aob_len(AOB_SET_FG), 62);
}

TEST(call_and_rip_targets_are_relative_to_the_next_instruction)
{
    uint8_t code[16] = { 0xE8, 0x10, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x15, 0xF0, 0xFF, 0xFF, 0xFF };
    CHECK(call_target(code) == (uintptr_t)code + 5 + 0x10);
    CHECK(rip_operand(code + 5) == (uintptr_t)code + 12 - 0x10);
}

// ===================================================================================== checks before attaching

TEST(all_four_hooks_attach_when_every_check_passes)
{
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, fake_resolve), 1);
    CHECK_EQ_INT(f_attached, 4);
    CHECK_EQ_INT(f_detached, 0);
    CHECK_EQ_INT(fg.framegen, 1);
    CHECK(fg.setFgOrig == (SetFgFn)(void *)f_setFg);
    CHECK(fg.o3dOrig == (O3dChangedFn)(void *)f_o3d);
    CHECK(fg.flagOrig == (FlagComputeFn)(void *)f_flag);
    CHECK(fg.root == (RootFn)(void *)f_lookup[0]);
    CHECK(fg.group == (FindFn)(void *)f_lookup[1]);
    CHECK(fg.var == (FindFn)(void *)f_lookup[2]);
    CHECK(fg.presetsName == &f_cnames[0]);
    CHECK(fg.fgName == &f_cnames[1]);
    CHECK(fg.dlssgOrig == (DlssgEnabledFn)(void *)f_dlssg);
    CHECK(fg.fsrEnabled == (FsrFgEnabledFn)(void *)f_fsrCode);
    CHECK_EQ_INT(fg.xess, 1);
    CHECK(strstr(ft_log, "hooks attached; DLSS, FSR and XeSS frame generation stays available") != NULL);
    CHECK_EQ_INT(f_setFg[0], 0xE9); // checked before the handler group's hook patched it
}

// A broken handler check: nothing is attached and the log names it.
static void expect_no_hooks(const char *why)
{
    memset(&fg, 0, sizeof(fg));
    ft_log[0] = 0;
    CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, fake_resolve), 0);
    CHECK_EQ_INT(f_attached, 0);
    CHECK_MSG(strstr(ft_log, why) != NULL, "log should say '%s', got: %s", why, ft_log);
    CHECK(strstr(ft_log, "nothing attached") != NULL);
}

// A broken frame generation check: only the handler's two hooks are attached (the menu squeeze still follows 3D Mode),
// and SetFrameGeneration passes everything through.
static void expect_handler_hooks_only(const char *why)
{
    memset(&fg, 0, sizeof(fg));
    ft_log[0] = 0;
    CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, fake_resolve), 0);
    CHECK_EQ_INT(f_attached, 2);
    CHECK_EQ_INT(fg.framegen, 0);
    CHECK_MSG(strstr(ft_log, why) != NULL, "log should say '%s', got: %s", why, ft_log);
    CHECK(strstr(ft_log, "only the 3D Mode handler is hooked") != NULL);
    // With the handler running and asking for Off, the game's rule applies and the squeeze hears "on".
    fg.setFgOrig = orig_set_fg;
    fg.o3dOrig = orig_o3d;
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK_EQ_INT(f_noted3d, 1);
}

TEST(nothing_attaches_when_a_hash_does_not_resolve)
{
    ft_begin();
    CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, resolve_nothing), 0);
    CHECK_EQ_INT(f_attached, 0);
    CHECK(strstr(ft_log, "didn't resolve into the game exe") != NULL);
}

TEST(nothing_attaches_when_the_handler_changed)
{
    ft_begin();
    f_o3d[0] = 0x48;
    expect_no_hooks("3D Mode handler: start doesn't match");
    ft_begin();
    memset(f_o3d + 0x188, 0xCC, 8);
    expect_no_hooks("the Frame Generation Off call isn't there");
    ft_begin();
    put(f_o3d + 0x400, AOB_O3D_FG_OFF); // twice: not sure which one
    expect_no_hooks("the Frame Generation Off call isn't there");
    ft_begin();
    put_rel32(f_o3d + 0x188 + aob_len(AOB_O3D_FG_OFF) + 4, f_other);
    expect_no_hooks("doesn't go to SetFrameGeneration");
    ft_begin();
    f_o3d[0x500] = 0xE8; // a second call to SetFrameGeneration
    put_rel32(f_o3d + 0x505, f_setFg);
    expect_no_hooks("it calls SetFrameGeneration somewhere else too");
}

TEST(only_the_handler_is_hooked_when_the_grey_or_set_frame_generation_changed)
{
    ft_begin();
    f_o3d[0x2FE + 7] = 0x0C; // bts eax,0Ch
    expect_handler_hooks_only("the greyed flag isn't set where expected");
    ft_begin();
    f_setFg[SET_FG_PRESETS] = 0x4C;
    expect_handler_hooks_only("SetFrameGeneration: start doesn't match");
    ft_begin();
    put_rel32(f_setFg + SET_FG_GROUP + 5, (uint8_t *)GetModuleHandleW(NULL) - 0x1000);
    expect_handler_hooks_only("its settings lookup points outside the exe");
}

TEST(only_the_handler_is_hooked_when_flag_compute_changed)
{
    ft_begin();
    f_flag[1] = 0;
    expect_handler_hooks_only("FlagCompute: start doesn't match");
    ft_begin();
    f_flag[0x5BC + 3] = 0x60; // another renderer offset
    expect_handler_hooks_only("the DLSS-G check isn't there");
    ft_begin();
    put_rel32(f_flag + 0x5BC + FLAG_DLSSG_CALL + 5, f_other);
    expect_handler_hooks_only("doesn't call IsDlssgEnabled");
    ft_begin();
    f_flag[0x6F0 + 14] = 0x59; // feature 89
    expect_handler_hooks_only("the stereo clear of feature 88 isn't there");
    ft_begin();
    f_flag[0x900] = 0xE8; // a second call to IsDlssgEnabled: its answer wouldn't be the DLSS-G decision
    put_rel32(f_flag + 0x905, f_dlssg);
    expect_handler_hooks_only("IsDlssgEnabled is called somewhere else in it too");
}

TEST(only_the_handler_is_hooked_when_the_sbsdebug_var_is_another_var)
{
    ft_begin();
    f_sbsVar[VAR_NAME / 8] = "StereoEnabled";
    expect_handler_hooks_only("isn't named [Odyssey3D] SBSDebug");
    ft_begin();
    f_sbsVar[VAR_GROUP / 8] = NULL;
    expect_handler_hooks_only("isn't named [Odyssey3D] SBSDebug");
}

TEST(a_failed_attach_detaches_the_hooks_of_its_group)
{
    for (int failAt = 1; failAt <= T_COUNT; failAt++) {
        ft_begin();
        memset(&fg, 0, sizeof(fg));
        f_attachFails = failAt;
        CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, fake_resolve), 0);
        // The handler group is hooks 1-2, the frame generation group 3-4.
        CHECK_EQ_INT(f_detached, failAt <= 2 ? failAt - 1 : failAt - 3);
        CHECK_EQ_INT(fg.framegen, 0);
        CHECK(strstr(ft_log, "couldn't hook") != NULL);
    }
}

TEST(framegen_load_without_red4ext_attaches_nothing)
{
    ft_begin();
    R4eSdk sdk = { 0 };
    sdk.hooking = &f_hooking;
    CHECK_EQ_INT(framegen_load(NULL, &sdk), 0); // no RED4ext.dll in this process
    CHECK_EQ_INT(f_attached, 0);
    CHECK_EQ_INT(framegen_load(NULL, NULL), 0);
}

// ===================================================================================== the 3D Mode handler

TEST(handler_with_sbs_keeps_dlss_frame_generation_and_its_greyed_flag)
{
    ft_begin();
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0); // the Off was skipped
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, 0);
    CHECK_EQ_INT(fg.handlerThread, 0);
    CHECK(strstr(ft_log, "Frame Generation kept and choosable") != NULL);
    CHECK(strstr(ft_log, "index 1 (applied 1) of 4, value 'DLSS'") != NULL);
    CHECK_EQ_INT(f_noted3d, 1); // the menu squeeze hears 3D Mode is on
}

TEST(handler_logs_a_menu_switch_though_the_game_clears_it)
{
    ft_begin();
    uint8_t event[8] = { 0, 1 };
    hook_o3d_changed(event);
    CHECK_EQ_INT(event[1], 0);
    CHECK_MSG(strstr(ft_log, "handler ran (event 1): 3D Mode on (SBSDebug on)") != NULL, "got: %s", ft_log);
}

TEST(handler_ungreys_frame_generation_greyed_by_an_earlier_run)
{
    // Saved as XESS, with the XeSS checks failed: turned Off and greyed at start-up. The next run sees Off and makes it
    // choosable again.
    ft_begin();
    fg.xess = 0;
    set_fg_value("XESS", 1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, FLAG_GREYED);
    set_fg_value("Off", 1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, 0);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_LIST_BUILT, FLAG_LIST_BUILT); // other flags kept
}

TEST(handler_does_what_the_game_does_when_the_setting_isnt_found)
{
    ft_begin();
    fg.var = fake_find_nothing;
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK(strstr(ft_log, "couldn't find the Frame Generation setting") != NULL);
    CHECK_EQ_INT(f_noted3d, 1);
}

TEST(handler_keeps_off_choosable_on_a_pc_without_dlss_frame_generation)
{
    // An RX 9000 (Off, FSR4, FSR3, XESS): the list has no DLSS, but FSR runs in stereo. Same on any PC without DLSS.
    ft_begin();
    set_fg_value("Off", 1);
    put_cstring(f_values[1], "FSR4");
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, 0);
}

TEST(handler_says_why_off_is_turned_off_when_the_list_has_neither_dlss_nor_fsr3)
{
    // A list (made up) of Off and XESS only, with the XeSS checks failed: nothing in it runs in 3D, though the FSR checks
    // passed.
    ft_begin();
    fg.xess = 0;
    set_fg_value("Off", 1);
    put_cstring(f_values[1], "XESS");
    put_cstring(f_values[2], "XESS");
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK(strstr(ft_log, "Frame Generation is Off, which can't run in 3D, so the game turns it off (the game offers "
                         "neither DLSS frame generation nor FSR3, and the XeSS checks at start-up failed)") != NULL);
    ft_begin(); // the same list with an Intel Arc's XESS left out
    set_fg_value("Off", 1);
    put_cstring(f_values[1], "XeSS2");
    put_cstring(f_values[2], "XeSS2");
    put_cstring(f_values[3], "XeSS2");
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK(strstr(ft_log, "(the game offers neither DLSS frame generation, FSR3 nor XESS)") != NULL);
}

TEST(handler_keeps_off_choosable_when_only_xess_runs_in_3d)
{
    // An Intel Arc whose FSR checks failed (made up: a list of Off and XESS), so only XeSS can be picked in 3D Mode.
    ft_begin();
    fg.fsrEnabled = NULL;
    set_fg_value("Off", 1);
    put_cstring(f_values[1], "XESS");
    put_cstring(f_values[2], "XESS");
    CHECK(keep_in_3d(f_var, "Off", 1));
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, 0);
}

TEST(handler_does_what_the_game_does_when_nothing_offered_runs_in_3d)
{
    // No DLSS in the list, and the FSR and XeSS checks failed at start-up.
    ft_begin();
    fg.fsrEnabled = NULL;
    fg.xess = 0;
    set_fg_value("Off", 1);
    put_cstring(f_values[1], "FSR4");
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, FLAG_GREYED);
    CHECK(strstr(ft_log, "Frame Generation is Off, which can't run in 3D, so the game turns it off (the game "
                         "offers no DLSS") != NULL);
}

TEST(handler_keeps_the_value_from_the_file_before_the_list_is_built)
{
    // At start-up the list isn't there yet, so it can't tell whether the PC offers DLSS: the saved value decides.
    ft_begin();
    set_fg_value("DLSS", -1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    ft_begin();
    set_fg_value("FSR3", -1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    ft_begin();
    set_fg_value("FSR4", -1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    ft_begin();
    set_fg_value("XESS", -1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    ft_begin();
    set_fg_value("XeSS2", -1); // a value it doesn't know
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
}

TEST(handler_does_what_the_game_does_with_a_value_it_doesnt_know)
{
    // A game update's new value: it may not run in stereo either.
    ft_begin();
    set_fg_value("DLSS", 1);
    put_cstring(f_values[3], "XeSS2");
    *(int32_t *)(f_var + SETTING_PENDING) = 3;
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK(strstr(ft_log, "Frame Generation is XeSS2, which can't run in 3D") != NULL);
}

TEST(keep_in_3d_allows_only_off_dlss_fsr_and_xess)
{
    ft_begin();
    set_fg_value("DLSS", 1);
    CHECK(keep_in_3d(f_var, "DLSS", 1));
    CHECK(keep_in_3d(f_var, "FSR3", 1));
    CHECK(keep_in_3d(f_var, "Off", 1));
    CHECK(keep_in_3d(f_var, "", 0)); // unreadable counts as Off
    CHECK(!keep_in_3d(f_var, "FSR4", 1)); // not on this PC's list (no RX 9000)
    CHECK(keep_in_3d(f_var, "XESS", 1));
    CHECK(!keep_in_3d(f_var, "DLSS2", 1));
    fg.xess = 0; // the XeSS checks failed
    CHECK(!keep_in_3d(f_var, "XESS", 1));
    fg.xess = 1;
    put_cstring(f_values[3], "XeSS2"); // a list without XESS
    CHECK(!keep_in_3d(f_var, "XESS", 1));
    fg.xess = 0;
    fg.fsrEnabled = NULL; // the FSR checks failed
    CHECK(!keep_in_3d(f_var, "FSR3", 1));
    CHECK(keep_in_3d(f_var, "Off", 1)); // DLSS can still be chosen
    put_cstring(f_values[1], "FSR4");   // no DLSS either: an RX 9000
    CHECK(!keep_in_3d(f_var, "Off", 1));
    CHECK(!keep_in_3d(f_var, "DLSS", 1));
    CHECK(!keep_in_3d(f_var, "FSR4", 1));
    fg.fsrEnabled = fake_fsr_enabled;
    CHECK(keep_in_3d(f_var, "Off", 1));
    CHECK(keep_in_3d(f_var, "FSR4", 1));
    CHECK(!keep_in_3d(f_var, "DLSS", 1));
    put_cstring(f_values[2], "FSR2"); // a list without FSR3
    CHECK(!keep_in_3d(f_var, "FSR3", 1));
    CHECK(!keep_in_3d(f_var, "Off", 1));
}

TEST(a_handler_run_inside_another_leaves_the_outer_ones_state)
{
    ft_begin();
    fg.offAsked = 1;
    fg.keptFlags = (uint32_t *)(f_var + SETTING_FLAGS);
    fg.handlerThread = 1234;
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(fg.offAsked, 1);
    CHECK(fg.keptFlags == (uint32_t *)(f_var + SETTING_FLAGS));
    CHECK_EQ_INT((int)fg.handlerThread, 1234);
}

TEST(handler_without_sbs_debug_does_what_the_game_does)
{
    ft_begin();
    set_sbs_debug(0);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK_EQ_INT(f_setFgIndex, 0);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, FLAG_GREYED);
    CHECK_EQ_INT(f_noted3d, 1); // the squeeze still follows the handler
}

TEST(handler_keeps_fsr_frame_generation)
{
    ft_begin();
    set_fg_value("FSR3", 1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, 0);
    CHECK(strstr(ft_log, "Frame Generation kept and choosable") != NULL);
    ft_begin();
    set_fg_value("Off", 1);
    put_cstring(f_values[1], "FSR4"); // an RX 9000's list: Off, FSR4, FSR3, XESS
    *(int32_t *)(f_var + SETTING_PENDING) = 1;
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    CHECK(strstr(ft_log, "value 'FSR4'") != NULL);
    CHECK(strstr(ft_log, "Frame Generation kept and choosable") != NULL);
}

TEST(handler_keeps_xess_frame_generation)
{
    ft_begin();
    set_fg_value("XESS", 1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, 0);
    CHECK(strstr(ft_log, "Frame Generation kept and choosable") != NULL);
}

TEST(handler_still_turns_off_xess_and_fsr_frame_generation_without_their_checks)
{
    ft_begin();
    fg.xess = 0;
    set_fg_value("XESS", 1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, FLAG_GREYED);
    CHECK(strstr(ft_log, "Frame Generation is XESS, which can't run in 3D, so the game turns it off (the XeSS checks "
                         "at start-up failed") != NULL);
    ft_begin();
    fg.fsrEnabled = NULL;
    set_fg_value("FSR3", 1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK(strstr(ft_log, "Frame Generation is FSR3, which can't run in 3D, so the game turns it off (the FSR checks") != NULL);
    ft_begin();
    fg.fsrEnabled = NULL;
    set_fg_value("Off", 1);
    put_cstring(f_values[1], "FSR4"); // an RX 9000's list: Off, FSR4, FSR3, XESS
    *(int32_t *)(f_var + SETTING_PENDING) = 1;
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK(strstr(ft_log, "Frame Generation is FSR4, which can't run in 3D, so the game turns it off (the FSR checks") != NULL);
}

TEST(handler_keeps_frame_generation_off_choosable)
{
    // Off stays choosable (not greyed), so DLSS can be chosen in 3D Mode; so does a value that can't be read.
    ft_begin();
    set_fg_value("Off", 1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, 0);
    ft_begin();
    set_fg_value("nothing", 1); // index -1
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    CHECK(strstr(ft_log, "value '?'") != NULL);
    ft_begin();
    fg.xess = 0;
    set_fg_value("XESS", 1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
}

TEST(frame_generation_value_is_read_from_the_list_or_from_the_file)
{
    char value[32];
    ft_begin();
    set_fg_value("FSR3", 1);
    REQUIRE(fg_value(f_var, value, sizeof(value)));
    CHECK_EQ_STR(value, "FSR3");
    set_fg_value("DLSS", -1); // start-up, before the list is built
    REQUIRE(fg_value(f_var, value, sizeof(value)));
    CHECK_EQ_STR(value, "DLSS");
    put_cstring(f_var + SETTING_FILE_VALUE, "A value long enough for the heap");
    char longValue[64];
    REQUIRE(fg_value(f_var, longValue, sizeof(longValue)));
    CHECK_EQ_STR(longValue, "A value long enough for the heap");
    CHECK(!fg_value(f_var, value, 8)); // doesn't fit
    *(uint32_t *)(f_var + SETTING_FLAGS) &= ~FLAG_FROM_FILE; // neither: nothing to read
    CHECK(!fg_value(f_var, value, sizeof(value)));
    set_fg_value("DLSS", 1);
    *(uint32_t *)(f_var + SETTING_VALUES + 0xC) = 1; // index past the end
    CHECK(!fg_value(f_var, value, sizeof(value)));
}

TEST(handler_with_3d_off_leaves_the_setting_alone)
{
    ft_begin();
    f_o3dSetsFgOff = 0; // the 3D-off branch clears the flag itself
    f_o3dGreys = 0;
    *(uint32_t *)(f_var + SETTING_FLAGS) = 0x1;
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 0);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS), 0x1);
    CHECK_EQ_INT(f_noted3d, 0); // the menu squeeze hears 3D Mode is off (the in-game test's inverted squeeze)
    set_sbs_debug(0);
    f_noted3d = -1;
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_noted3d, 0);
}

TEST(set_frame_generation_outside_the_handler_passes_through)
{
    ft_begin();
    CHECK_EQ_INT(hook_set_fg(0, 3), 1); // graphics preset or reset to defaults
    CHECK_EQ_INT(f_setFgCalls, 1);
    fg.handlerThread = GetCurrentThreadId();
    hook_set_fg(1, 1); // not "Off"
    CHECK_EQ_INT(f_setFgCalls, 2);
    CHECK(fg.keptFlags == NULL);
    fg.handlerThread = GetCurrentThreadId() + 1; // the handler is running on another thread
    hook_set_fg(0, 1);
    CHECK_EQ_INT(f_setFgCalls, 3);
}

// ===================================================================================== FlagCompute

static uint64_t run_flags(void)
{
    uint64_t flags[2] = { 0, 0 };
    uint64_t *ret = hook_flag_compute(NULL, flags, NULL, NULL);
    CHECK(ret == flags); // a lost return value crashed the game in the first in-game test
    return flags[1];
}

static uint64_t f_otherFlags[2];
static uint64_t *orig_flag_compute_other_buffer(uint8_t *renderer, uint64_t *flags, void *frame, void *settings)
{
    orig_flag_compute(renderer, f_otherFlags, frame, settings);
    return f_otherFlags;
}

TEST(flag_compute_sets_the_bit_where_the_caller_reads_it)
{
    ft_begin();
    fg.flagOrig = orig_flag_compute_other_buffer;
    uint64_t flags[2] = { 0, 0 };
    CHECK(hook_flag_compute(NULL, flags, NULL, NULL) == f_otherFlags);
    CHECK(f_otherFlags[1] == (FEATURE_BIT(FEATURE_STEREO) | FEATURE_BIT(FEATURE_DLSSG)));
    CHECK(flags[1] == 0);
}

static uint64_t *orig_flag_compute_nested(uint8_t *renderer, uint64_t *flags, void *frame, void *settings)
{
    fg.flagOrig = orig_flag_compute;
    uint64_t inner[2];
    f_dlssgOn = 0;
    hook_flag_compute(renderer, inner, frame, settings); // asks and gets "off"
    f_dlssgOn = 1;
    return orig_flag_compute(renderer, flags, frame, settings);
}

TEST(flag_compute_inside_flag_compute_keeps_each_ones_answer)
{
    ft_begin();
    fg.flagOrig = orig_flag_compute_nested;
    CHECK(run_flags() == (FEATURE_BIT(FEATURE_STEREO) | FEATURE_BIT(FEATURE_DLSSG)));
    CHECK_EQ_INT((int)thread_state(), TS_OUTSIDE);
}

TEST(thread_state_keeps_the_last_error)
{
    ft_begin();
    SetLastError(1234);
    set_thread_state(TS_NOT_ASKED);
    CHECK_EQ_INT((int)thread_state(), TS_NOT_ASKED);
    CHECK_EQ_INT((int)GetLastError(), 1234);
    set_thread_state(TS_OUTSIDE);
}

TEST(thunks_have_unwind_info)
{
    // Without it an exception thrown through a hook (or a crash dump's stack) can't get past the thunk.
    void *thunks[] = { (void *)bd3d_thunk_o3d_changed, (void *)bd3d_thunk_set_fg, (void *)bd3d_thunk_dlssg_enabled,
                       (void *)bd3d_thunk_flag_compute, (void *)bd3d_thunk_plane_override,
                       (void *)bd3d_thunk_weapon_sep };
    for (int i = 0; i < 6; i++) {
        DWORD64 base = 0;
        CHECK_MSG(RtlLookupFunctionEntry((DWORD64)(uintptr_t)thunks[i], &base, NULL) != NULL, "thunk %d", i);
    }
}

TEST(stereo_frames_get_dlssg_back_when_the_game_would_run_it)
{
    ft_begin();
    CHECK(run_flags() == (FEATURE_BIT(FEATURE_STEREO) | FEATURE_BIT(FEATURE_DLSSG)));
    CHECK_EQ_INT((int)fg.dlssgCount.restored, 1);
    CHECK(strstr(ft_log, "first one") != NULL);
    run_flags();
    CHECK_EQ_INT((int)fg.dlssgCount.restored, 2);
    CHECK_EQ_INT((int)fg.fsrCount.restored, 0); // no renderer passed: no FSR object to ask
}

static uint64_t run_flags_with_renderer(void)
{
    uint64_t flags[2] = { 0, 0 };
    CHECK(hook_flag_compute(f_renderer, flags, NULL, NULL) == flags);
    return flags[1];
}

TEST(stereo_frames_get_fsr_back_when_the_game_would_run_it)
{
    ft_begin();
    f_dlssgOn = 0; // FSR frame generation instead of DLSS-G
    CHECK(run_flags_with_renderer() == (FEATURE_BIT(FEATURE_STEREO) | FEATURE_BIT(FEATURE_FSR_FG)));
    CHECK_EQ_INT((int)fg.fsrCount.restored, 1);
    CHECK_EQ_INT((int)fg.dlssgCount.restored, 0);
    CHECK(strstr(ft_log, "FSR frame generation running on a 3D frame (first one)") != NULL);
}

TEST(flag_compute_leaves_feature_89_alone_otherwise)
{
    uint64_t stereo = FEATURE_BIT(FEATURE_STEREO), fsr = FEATURE_BIT(FEATURE_FSR_FG);
    ft_begin();
    f_dlssgOn = 0;
    f_fsr[0x5A] = 1; // FSR's own "off" flag
    CHECK(run_flags_with_renderer() == stereo);
    ft_begin();
    f_dlssgOn = 0;
    f_eligible = 0; // the view didn't pass the frame generation checks
    CHECK(run_flags_with_renderer() == stereo);
    ft_begin();
    f_dlssgOn = 0;
    fg.fsrEnabled = NULL; // the FSR checks failed at start-up
    CHECK(run_flags_with_renderer() == stereo);
    ft_begin();
    f_dlssgOn = 0;
    set_sbs_debug(0); // a real 3D monitor
    CHECK(run_flags_with_renderer() == stereo);
    ft_begin();
    f_dlssgOn = 0;
    f_flagsAfterOrig[1] = 0; // 2D: the game sets 89 itself
    CHECK(run_flags_with_renderer() == 0);
    ft_begin();
    f_dlssgOn = 0;
    f_flagsAfterOrig[1] = stereo | fsr; // already on
    run_flags_with_renderer();
    CHECK_EQ_INT((int)fg.fsrCount.restored, 0);
    ft_begin();
    f_dlssgOn = 0;
    *(uint8_t **)(f_renderer + RENDERER_FSR) = NULL; // no FSR object
    CHECK(run_flags_with_renderer() == stereo);
}

TEST(stereo_frames_get_xess_back_when_the_game_would_run_it)
{
    ft_begin();
    f_dlssgOn = 0;      // XeSS frame generation instead of DLSS-G
    f_fsr[0x59] = 0;    // and FSR off
    f_xess[XESS_FG_ON] = 1;
    CHECK(run_flags_with_renderer() == (FEATURE_BIT(FEATURE_STEREO) | FEATURE_BIT(FEATURE_XESS_FG)));
    CHECK_EQ_INT((int)fg.xessCount.restored, 1);
    CHECK_EQ_INT((int)fg.fsrCount.restored, 0);
    CHECK(strstr(ft_log, "XeSS frame generation running on a 3D frame (first one)") != NULL);
}

TEST(flag_compute_leaves_feature_75_alone_otherwise)
{
    uint64_t stereo = FEATURE_BIT(FEATURE_STEREO), xess = FEATURE_BIT(FEATURE_XESS_FG);
    for (int k = 0; k < 6; k++) {
        ft_begin();
        f_dlssgOn = 0;
        f_fsr[0x59] = 0;
        f_xess[XESS_FG_ON] = 1;
        uint64_t want = stereo;
        if (k == 0) f_xess[XESS_FG_ON] = 0;                           // XeSS frame generation isn't on
        if (k == 1) f_eligible = 0;                                   // the view didn't pass the checks
        if (k == 2) fg.xess = 0;                                      // the XeSS checks failed at start-up
        if (k == 3) set_sbs_debug(0);                                 // a real 3D monitor
        if (k == 4) *(uint8_t **)(f_renderer + RENDERER_XESS) = NULL; // no XeSS object
        if (k == 5) f_flagsAfterOrig[1] = want = 0;                   // 2D: the game sets 75 itself
        CHECK_MSG(run_flags_with_renderer() == want, "case %d", k);
        CHECK_MSG(fg.xessCount.restored == 0, "case %d counted", k);
    }
    ft_begin();
    f_dlssgOn = 0;
    f_xess[XESS_FG_ON] = 1;
    f_flagsAfterOrig[1] = stereo | xess; // already on
    run_flags_with_renderer();
    CHECK_EQ_INT((int)fg.xessCount.restored, 0);
}

TEST(flag_compute_changes_nothing_otherwise)
{
    ft_begin();
    f_flagsAfterOrig[1] = 0; // 2D frame
    CHECK(run_flags() == 0);
    ft_begin();
    set_sbs_debug(0); // a real 3D monitor
    CHECK(run_flags() == FEATURE_BIT(FEATURE_STEREO));
    ft_begin();
    f_eligible = 0; // the game's other checks said no for this view, so IsDlssgEnabled isn't asked
    CHECK(run_flags() == FEATURE_BIT(FEATURE_STEREO));
    ft_begin();
    f_dlssgOn = 0; // DLSS-G not loaded or not on
    CHECK(run_flags() == FEATURE_BIT(FEATURE_STEREO));
    ft_begin();
    f_flagsAfterOrig[1] |= FEATURE_BIT(FEATURE_DLSSG); // already on
    run_flags();
    CHECK_EQ_INT((int)fg.dlssgCount.restored, 0);
}

TEST(is_dlssg_enabled_only_counts_inside_flag_compute)
{
    ft_begin();
    CHECK_EQ_INT(hook_dlssg_enabled(NULL), 1); // the per-frame DLSS-G code asking
    f_eligible = 0;
    CHECK(run_flags() == FEATURE_BIT(FEATURE_STEREO));
    CHECK_EQ_INT(f_dlssgCalls, 1);
}

static DWORD WINAPI eligible_view_thread(void *arg)
{
    f_pauseThread = GetCurrentThreadId();
    *(uint64_t *)arg = run_flags();
    return 0;
}

TEST(each_thread_gets_its_own_dlssg_decision)
{
    ft_begin();
    // A worker thread's eligible view is paused inside FlagCompute, right after IsDlssgEnabled said yes. A view on
    // this thread that the game didn't find eligible must not pick up that answer.
    f_asked = CreateEventW(NULL, TRUE, FALSE, NULL);
    f_resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    uint64_t other = 0;
    HANDLE t = CreateThread(NULL, 0, eligible_view_thread, &other, 0, NULL);
    REQUIRE(t != NULL);
    REQUIRE(WaitForSingleObject(f_asked, 10000) == WAIT_OBJECT_0);
    f_eligible = 0;
    CHECK(run_flags() == FEATURE_BIT(FEATURE_STEREO));
    SetEvent(f_resume);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CloseHandle(f_asked);
    CloseHandle(f_resume);
    f_pauseThread = 0;
    CHECK(other == (FEATURE_BIT(FEATURE_STEREO) | FEATURE_BIT(FEATURE_DLSSG)));
}

// ===================================================================================== the register-keeping thunks

// Calls thunk(rcx, rdx, r8, r9) with known values in rcx, rdx (rdxValue), r8-r11 and xmm0-xmm5, the way the game's
// code may keep using them after the call, and writes what they and rax hold afterwards: out[0..5] rcx, rdx, r8-r11,
// out[6] rax, out[7..12] the low halves of xmm0-xmm5.
void ft_call_thunk(void *thunk, uint64_t *out, uint64_t rdxValue);
__asm__(".text\n.globl ft_call_thunk\nft_call_thunk:\n"
        "push %rdi\npush %rbx\npush %rsi\nsub $0x20, %rsp\n" // 16-aligned again, with shadow space
        "mov %rcx, %rbx\nmov %rdx, %rdi\nmov %r8, %rsi\n"
        "movabs $0x7777777777777777, %rax\nmovq %rax, %xmm0\nmovabs $0x8888888888888888, %rax\nmovq %rax, %xmm1\n"
        "movabs $0x9999999999999999, %rax\nmovq %rax, %xmm2\nmovabs $0xAAAAAAAAAAAAAAAA, %rax\nmovq %rax, %xmm3\n"
        "movabs $0xBBBBBBBBBBBBBBBB, %rax\nmovq %rax, %xmm4\nmovabs $0xCCCCCCCCCCCCCCCC, %rax\nmovq %rax, %xmm5\n"
        "movabs $0x1111111111111111, %rcx\nmov %rsi, %rdx\nmovabs $0x3333333333333333, %r8\n"
        "movabs $0x4444444444444444, %r9\nmovabs $0x5555555555555555, %r10\nmovabs $0x6666666666666666, %r11\n"
        "call *%rbx\n"
        "mov %rcx, 0(%rdi)\nmov %rdx, 8(%rdi)\nmov %r8, 16(%rdi)\nmov %r9, 24(%rdi)\nmov %r10, 32(%rdi)\n"
        "mov %r11, 40(%rdi)\nmov %rax, 48(%rdi)\n"
        "movq %xmm0, 56(%rdi)\nmovq %xmm1, 64(%rdi)\nmovq %xmm2, 72(%rdi)\nmovq %xmm3, 80(%rdi)\n"
        "movq %xmm4, 88(%rdi)\nmovq %xmm5, 96(%rdi)\n"
        "add $0x20, %rsp\npop %rsi\npop %rbx\npop %rdi\nret\n");

static float fake_weapon_sep(void *settings) { (void)settings; return 0.017f; } // Weapon Depth 1.0

// Returns rax.
static uint64_t check_thunk_registers(void *thunk, uint64_t rdxValue, uint64_t *xmm0)
{
    uint64_t out[13] = { 0 };
    ft_call_thunk(thunk, out, rdxValue);
    uint64_t want[13] = { 0x1111111111111111ull, rdxValue, 0x3333333333333333ull, 0x4444444444444444ull,
                          0x5555555555555555ull, 0x6666666666666666ull, 0, 0x7777777777777777ull,
                          0x8888888888888888ull, 0x9999999999999999ull, 0xAAAAAAAAAAAAAAAAull,
                          0xBBBBBBBBBBBBBBBBull, 0xCCCCCCCCCCCCCCCCull };
    for (int i = 0; i < 13; i++)
        if (i != 6 && !(i == 7 && xmm0))
            CHECK_MSG(out[i] == want[i], "register %d changed to %llx", i, (unsigned long long)out[i]);
    if (xmm0) *xmm0 = out[7];
    return out[6];
}

static uint64_t check_thunk_keeps_registers(void *thunk, uint64_t rdxValue)
{
    return check_thunk_registers(thunk, rdxValue, NULL);
}

// The same for a thunk whose hook returns a float: xmm0 (register 7) is its answer.
static float check_float_thunk_keeps_registers(void *thunk, uint64_t rdxValue)
{
    uint64_t xmm0 = 0;
    check_thunk_registers(thunk, rdxValue, &xmm0);
    float f;
    memcpy(&f, &xmm0, sizeof(f));
    return f;
}

TEST(thunks_keep_the_callers_registers_and_return_the_result)
{
    ft_begin();
    // IsDlssgEnabled: FlagCompute writes [rcx+0x1F2] right after calling it (the second in-game test's crash).
    f_dlssgOn = 1;
    CHECK_EQ_INT((int)(check_thunk_keeps_registers((void *)bd3d_thunk_dlssg_enabled, 0x2222222222222222ull) & 0xFF), 1);
    CHECK_EQ_INT(f_dlssgCalls, 1);
    // FlagCompute: its caller reads the flags through the returned pointer (the first in-game test's crash). The
    // renderer (rcx) is a test value here, so the FSR and XeSS parts, which read the renderer's objects, stay out.
    fg.fsrEnabled = NULL;
    fg.xess = 0;
    uint64_t flags[2] = { 0, 0 };
    CHECK(check_thunk_keeps_registers((void *)bd3d_thunk_flag_compute, (uint64_t)(uintptr_t)flags) ==
          (uint64_t)(uintptr_t)flags);
    CHECK(flags[1] == (FEATURE_BIT(FEATURE_STEREO) | FEATURE_BIT(FEATURE_DLSSG)));
    // SetFrameGeneration outside the handler, passed through.
    CHECK_EQ_INT((int)(check_thunk_keeps_registers((void *)bd3d_thunk_set_fg, 0x2222222222222222ull) & 0xFF), 1);
    CHECK_EQ_INT(f_setFgCalls, 1);
    // The handler with SBSDebug on: it calls SetFrameGeneration's thunk, which answers 1 itself (kept).
    check_thunk_keeps_registers((void *)bd3d_thunk_o3d_changed, 0x2222222222222222ull);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK(strstr(ft_log, "kept and choosable") != NULL);
    // The plane override (rcx is a test value: the fake original doesn't read the override): the eyes are put back.
    CHECK(check_thunk_keeps_registers((void *)bd3d_thunk_plane_override, (uint64_t)(uintptr_t)f_centre) == 0x5678);
    CHECK_EQ_INT(f_builds, 2);
    // The weapon separation getter (rcx is a test value: the fake original doesn't read it).
    fg.sepOrig = fake_weapon_sep;
    CHECK(check_float_thunk_keeps_registers((void *)bd3d_thunk_weapon_sep, 0x2222222222222222ull) == 0.017f);
}

// ===================================================================================== one function at a time

TEST(aob_at_compares_only_the_unmasked_bytes)
{
    const uint8_t code[] = { 0x48, 0x8B, 0x05, 0x11 };
    uint8_t bytes[8], mask[8];
    int n = aob_parse("48 ?? 05", bytes, mask, 8);
    REQUIRE_EQ_INT(n, 3);
    CHECK_EQ_INT(mask[1], 0);
    CHECK(aob_at(code, bytes, mask, n));
    CHECK(!aob_at(code + 1, bytes, mask, n));
    CHECK_EQ_INT(aob_parse("48 8B 05 11", bytes, mask, 3), 0); // longer than max
}

TEST(count_calls_counts_only_calls_to_the_target)
{
    static uint8_t code[32]; // in the image, within rel32 reach of the targets
    memset(code, 0x90, sizeof(code));
    code[0] = code[10] = code[20] = 0xE8;
    put_rel32(code + 5, f_dlssg);
    put_rel32(code + 15, f_dlssg);
    put_rel32(code + 25, f_setFg);
    CHECK_EQ_INT(count_calls(code, sizeof(code), (uintptr_t)f_dlssg), 2);
    CHECK_EQ_INT(count_calls(code, sizeof(code), (uintptr_t)f_setFg), 1);
    CHECK_EQ_INT(count_calls(code, 24, (uintptr_t)f_setFg), 0); // the call doesn't fit
}

TEST(in_exe_wants_the_whole_range_inside_the_exe_image)
{
    const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
    uintptr_t end = (uintptr_t)base + tu_nt((HMODULE)base)->OptionalHeader.SizeOfImage;
    CHECK(in_exe((uintptr_t)f_o3d, sizeof(f_o3d)));
    CHECK(in_exe(end - 1, 1));
    CHECK(!in_exe(end - 1, 2));
    CHECK(!in_exe(end, 1));
    CHECK(!in_exe((uintptr_t)base - 1, 1));
    void *heap = malloc(16);
    CHECK(!in_exe((uintptr_t)heap, 16));
    free(heap);
}

TEST(readable_and_text_is_need_committed_memory)
{
    CHECK(readable(f_o3d, sizeof(f_o3d)));
    CHECK(!readable(NULL, 1));
    CHECK(!readable((void *)(uintptr_t)8, 1));
    CHECK(text_is("SBSDebug", "SBSDebug"));
    CHECK(!text_is("SBSDebug2", "SBSDebug"));
    CHECK(!text_is("SBS", "SBSDebug"));
    CHECK(!text_is(NULL, "SBSDebug"));
}

TEST(sbs_debug_reads_the_config_vars_value)
{
    ft_begin();
    CHECK_EQ_INT(sbs_debug(), 1);
    set_sbs_debug(0);
    CHECK_EQ_INT(sbs_debug(), 0);
    set_sbs_debug(1);
}

TEST(fg_var_is_found_the_way_set_frame_generation_finds_it)
{
    ft_begin();
    CHECK_EQ_PTR(fg_var(), f_var);
    fg.var = fake_find_nothing;
    CHECK(fg_var() == NULL);
}

TEST(read_cstring_reads_inline_and_heap_text)
{
    uint8_t s[CSTRING_SIZE];
    char out[32];
    put_cstring(s, "DLSS");
    CHECK_EQ_INT(read_cstring(s, out, sizeof(out)), 1);
    CHECK_EQ_STR(out, "DLSS");
    put_cstring(s, "a value longer than sixteen");
    CHECK_EQ_INT(read_cstring(s, out, sizeof(out)), 1);
    CHECK_EQ_STR(out, "a value longer than sixteen");
    CHECK_EQ_INT(read_cstring(s, out, 8), 0); // doesn't fit
    put_cstring(s, "");
    CHECK_EQ_INT(read_cstring(s, out, sizeof(out)), 1);
    CHECK_EQ_STR(out, "");
    *(const char **)s = (const char *)(uintptr_t)8; // a heap pointer that can't be read
    *(uint32_t *)(s + CSTRING_LEN_FLAGS) = 4 | CSTRING_HEAP;
    CHECK_EQ_INT(read_cstring(s, out, sizeof(out)), 0);
    CHECK_EQ_INT(read_cstring(NULL, out, sizeof(out)), 0);
}

TEST(offers_looks_through_the_built_list)
{
    ft_begin();
    set_fg_value("Off", 1);
    CHECK_EQ_INT(offers(f_var, "DLSS"), 1);
    CHECK_EQ_INT(offers(f_var, "FSR3"), 1);
    CHECK_EQ_INT(offers(f_var, "FSR4"), 0);
    put_cstring(f_values[1], "FSR4"); // a PC without DLSS frame generation
    CHECK_EQ_INT(offers(f_var, "DLSS"), 0);
    CHECK_EQ_INT(offers(f_var, "FSR4"), 1);
    *(uint32_t *)(f_var + SETTING_VALUES + 0xC) = 17; // a size this can't trust
    CHECK_EQ_INT(offers(f_var, "FSR4"), 0);
    set_fg_value("Off", -1); // before the list is built it can't tell
    CHECK_EQ_INT(offers(f_var, "DLSS"), 1);
}

TEST(ungrey_clears_only_the_greyed_flag)
{
    ft_begin();
    uint32_t flags = FLAG_GREYED | FLAG_LIST_BUILT | 1;
    ungrey(&flags);
    CHECK_EQ_INT(flags, FLAG_LIST_BUILT | 1);
    CHECK(strstr(ft_log, "Frame Generation flags 0x841 -> 0x41 (not greyed)") != NULL);
    ungrey(&flags);
    CHECK_EQ_INT(flags, FLAG_LIST_BUILT | 1);
}

static DWORD f_threadInHandler;
static void orig_o3d_marks(uint8_t *event)
{
    f_threadInHandler = fg.handlerThread;
    fg.offAsked = 1;
    fg.keptFlags = (uint32_t *)(f_var + SETTING_FLAGS);
}

TEST(run_handler_marks_the_thread_while_the_handler_runs)
{
    ft_begin();
    fg.o3dOrig = orig_o3d_marks;
    fg.offAsked = 7; // an outer run's marks
    fg.handlerThread = 1234;
    int offAsked = 0;
    uint32_t *kept = NULL;
    run_handler(NULL, &offAsked, &kept);
    CHECK_EQ_INT(f_threadInHandler, GetCurrentThreadId());
    CHECK_EQ_INT(offAsked, 1);
    CHECK_EQ_PTR(kept, f_var + SETTING_FLAGS);
    CHECK_EQ_INT(fg.offAsked, 7);
    CHECK(fg.keptFlags == NULL);
    CHECK_EQ_INT(fg.handlerThread, 1234);
}

TEST(only_the_handlers_own_off_call_counts)
{
    ft_begin();
    CHECK(!is_handlers_off(0, 1)); // no handler running
    fg.handlerThread = GetCurrentThreadId();
    CHECK(is_handlers_off(0, 1));
    CHECK(!is_handlers_off(1, 1));
    CHECK(!is_handlers_off(0, 0));
    fg.handlerThread = GetCurrentThreadId() + 1; // another thread's
    CHECK(!is_handlers_off(0, 1));
}

TEST(keep_frame_generation_keeps_off_dlss_or_fsr_while_sbsdebug_is_on)
{
    ft_begin();
    CHECK_EQ_INT(keep_frame_generation(), 1);
    CHECK_EQ_PTR(fg.keptFlags, f_var + SETTING_FLAGS);
    struct { int framegen, sbs, found; const char *value; const char *log; } cases[] = {
        { 0, 1, 1, "DLSS", NULL },
        { 1, 0, 1, "DLSS", NULL },
        { 1, 1, 0, "DLSS", "couldn't find the Frame Generation setting" },
        { 1, 1, 1, "XESS", "Frame Generation is XESS, which can't run in 3D" },
    };
    for (int k = 0; k < 4; k++) {
        ft_begin();
        fg.xess = 0; // so XESS is a value that can't run in 3D
        fg.framegen = cases[k].framegen;
        set_sbs_debug(cases[k].sbs);
        if (!cases[k].found) fg.var = fake_find_nothing;
        set_fg_value(cases[k].value, 1);
        CHECK_MSG(keep_frame_generation() == 0, "case %d kept it", k);
        CHECK_MSG(fg.keptFlags == NULL, "case %d set the flags to ungrey", k);
        if (cases[k].log) CHECK_MSG(strstr(ft_log, cases[k].log) != NULL, "case %d: no \"%s\" in:\n%s", k, cases[k].log, ft_log);
        set_sbs_debug(1);
    }
}

TEST(dlssg_is_restored_only_when_only_stereo_took_it_away)
{
    ft_begin();
    uint64_t stereo = FEATURE_BIT(FEATURE_STEREO), dlssg = FEATURE_BIT(FEATURE_DLSSG);
    CHECK(should_restore_dlssg(TS_DLSSG_ON, stereo));
    CHECK(!should_restore_dlssg(TS_DLSSG_OFF, stereo));
    CHECK(!should_restore_dlssg(TS_NOT_ASKED, stereo));
    CHECK(!should_restore_dlssg(TS_DLSSG_ON, 0));              // a flat view
    CHECK(!should_restore_dlssg(TS_DLSSG_ON, stereo | dlssg)); // already on
    set_sbs_debug(0);
    CHECK(!should_restore_dlssg(TS_DLSSG_ON, stereo));
    set_sbs_debug(1);
}

TEST(fsr_is_restored_for_any_view_that_passed_the_checks)
{
    ft_begin();
    uint64_t stereo = FEATURE_BIT(FEATURE_STEREO), fsr = FEATURE_BIT(FEATURE_FSR_FG);
    CHECK(should_restore_fsr(TS_DLSSG_OFF, stereo, f_renderer));
    CHECK(should_restore_fsr(TS_DLSSG_ON, stereo, f_renderer)); // the game asks both; it doesn't run both
    CHECK(!should_restore_fsr(TS_NOT_ASKED, stereo, f_renderer));
    CHECK(!should_restore_fsr(TS_OUTSIDE, stereo, f_renderer));
    CHECK(!should_restore_fsr(TS_DLSSG_OFF, 0, f_renderer));
    CHECK(!should_restore_fsr(TS_DLSSG_OFF, stereo | fsr, f_renderer));
    CHECK(!should_restore_fsr(TS_DLSSG_OFF, stereo, NULL));
    f_fsr[0x59] = 0;
    CHECK(!should_restore_fsr(TS_DLSSG_OFF, stereo, f_renderer));
}

TEST(restored_frames_are_logged_first_and_then_once_a_minute)
{
    ft_begin();
    count_restored(&fg.dlssgCount, "DLSS");
    CHECK(strstr(ft_log, "DLSS frame generation running on a 3D frame (first one)") != NULL);
    count_restored(&fg.dlssgCount, "DLSS");
    CHECK(strstr(ft_log, "frames with DLSS frame generation in 3D") == NULL);
    fg.dlssgCount.lastSummary = GetTickCount() - 61000;
    count_restored(&fg.dlssgCount, "DLSS");
    CHECK(strstr(ft_log, "3 frames with DLSS frame generation in 3D (3 in the last minute)") != NULL);
    CHECK_EQ_INT((int)fg.dlssgCount.lastRestored, 3);
    count_restored(&fg.fsrCount, "FSR"); // counted on its own
    CHECK(strstr(ft_log, "FSR frame generation running on a 3D frame (first one)") != NULL);
}

TEST(fail_logs_and_returns_0)
{
    ft_begin();
    CHECK_EQ_INT(fail("something moved"), 0);
    CHECK(strstr(ft_log, "framegen: something moved (game update?)") != NULL);
}

TEST(check_handler_finds_the_handler_and_its_off_call)
{
    ft_begin();
    uint8_t *t[T_COUNT] = { 0 };
    CHECK_EQ_INT(check_handler(fake_resolve, t), 1);
    CHECK_EQ_PTR(t[T_O3D], f_o3d);
    CHECK_EQ_PTR(t[T_SET_FG], f_setFg);
    ft_begin();
    f_o3d[0x400] = 0xE8; // a second SetFrameGeneration call
    put_rel32(f_o3d + 0x405, f_setFg);
    CHECK_EQ_INT(check_handler(fake_resolve, t), 0);
    CHECK(strstr(ft_log, "it calls SetFrameGeneration somewhere else too") != NULL);
}

TEST(check_settings_lookup_reads_set_frame_generations_lookup)
{
    ft_begin();
    SettingsLookup l = { 0 };
    CHECK_EQ_INT(check_settings_lookup(f_setFg, &l), 1);
    CHECK(l.root == (uintptr_t)f_lookup[0]);
    CHECK(l.group == (uintptr_t)f_lookup[1]);
    CHECK(l.var == (uintptr_t)f_lookup[2]);
    CHECK(l.presets == (uintptr_t)&f_cnames[0]);
    CHECK(l.name == (uintptr_t)&f_cnames[1]);
    const uint8_t *base = (const uint8_t *)GetModuleHandleW(NULL);
    put_rel32(f_setFg + SET_FG_VAR + 5, base + tu_nt((HMODULE)base)->OptionalHeader.SizeOfImage + 16);
    CHECK_EQ_INT(check_settings_lookup(f_setFg, &l), 0);
    CHECK(strstr(ft_log, "its settings lookup points outside the exe") != NULL);
}

TEST(check_flag_compute_wants_one_dlssg_call_before_the_stereo_clear)
{
    ft_begin();
    CHECK_EQ_INT(check_flag_compute(f_flag, f_dlssg), 1);
    CHECK_EQ_INT(check_flag_compute(f_flag, f_other), 0);
    CHECK(strstr(ft_log, "the DLSS-G check doesn't call IsDlssgEnabled") != NULL);
    ft_begin();
    memset(f_flag + 0x6F0, 0xCC, (size_t)aob_len(AOB_FLAG_CLEAR));
    put(f_flag + 0x100, AOB_FLAG_CLEAR);
    CHECK_EQ_INT(check_flag_compute(f_flag, f_dlssg), 0);
    CHECK(strstr(ft_log, "the stereo clear comes before the DLSS-G check") != NULL);
    ft_begin();
    f_flag[0x900] = 0xE8;
    put_rel32(f_flag + 0x905, f_dlssg);
    CHECK_EQ_INT(check_flag_compute(f_flag, f_dlssg), 0);
    CHECK(strstr(ft_log, "IsDlssgEnabled is called somewhere else in it too") != NULL);
}

TEST(check_fsr_finds_is_fsr_fg_enabled_between_the_dlssg_check_and_the_clears)
{
    ft_begin();
    CHECK_EQ_PTR((void *)check_fsr(f_flag), f_fsrCode);
    struct { int at; uint8_t byte; const char *log; } cases[] = {
        { 0x660 + 9, 0x60, "the FSR frame generation check isn't there" },                 // another renderer offset
        { -1, 0, "the FSR frame generation check isn't between" },                          // moved before the DLSS-G check
        { -2, 0, "the FSR frame generation check doesn't call IsFsrFgEnabled" },           // its target changed
        { -3, 0, "IsFsrFgEnabled is called somewhere else in it too" },
        { 0x707 + 19, 0x58, "the stereo clear of feature 89 isn't there" },
        { -4, 0, "the stereo clear of feature 89 isn't after the one of 88" },
    };
    for (int k = 0; k < 6; k++) {
        ft_begin();
        if (cases[k].at >= 0) f_flag[cases[k].at] = cases[k].byte;
        if (cases[k].at == -1) {
            memset(f_flag + 0x660, 0xCC, (size_t)aob_len(AOB_FLAG_FSR));
            put(f_flag + 0x200, AOB_FLAG_FSR);
            put_rel32(f_flag + 0x200 + FLAG_FSR_CALL + 5, f_fsrCode);
        }
        if (cases[k].at == -2) f_fsrCode[0x3] = 1;
        if (cases[k].at == -3) {
            f_flag[0x900] = 0xE8;
            put_rel32(f_flag + 0x905, f_fsrCode);
        }
        if (cases[k].at == -4) {
            memset(f_flag + 0x707, 0xCC, (size_t)aob_len(AOB_FLAG_CLEAR_FSR));
            put(f_flag + 0x100, AOB_FLAG_CLEAR_FSR);
        }
        CHECK_MSG(check_fsr(f_flag) == NULL, "case %d passed", k);
        CHECK_MSG(strstr(ft_log, cases[k].log) != NULL, "case %d: no \"%s\" in:\n%s", k, cases[k].log, ft_log);
        CHECK(strstr(ft_log, "FSR frame generation stays off in 3D Mode (DLSS isn't affected)") != NULL);
    }
}

TEST(a_failed_fsr_check_leaves_dlss_attached)
{
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    f_fsrCode[0] = 0x90;
    CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, fake_resolve), 1);
    CHECK_EQ_INT(f_attached, 4);
    CHECK(fg.fsrEnabled == NULL);
    CHECK_EQ_INT(fg.xess, 1);
    CHECK(strstr(ft_log, "hooks attached; DLSS and XeSS frame generation stays available") != NULL);
}

TEST(check_xess_finds_the_xess_check_between_the_dlssg_check_and_the_clears)
{
    ft_begin();
    CHECK_EQ_INT(check_xess(f_flag), 1);
    struct { int at; uint8_t byte; const char *log; } cases[] = {
        { 0x6A0 + 3, 0x68, "the XeSS frame generation check isn't there" },  // another renderer offset
        { 0x6A0 + 11, 0xDE, "the XeSS frame generation check isn't there" }, // another flag
        { -1, 0, "the XeSS frame generation check isn't between" },          // moved after the clear
        { 0x730 + 19, 0x4C, "the stereo clear of feature 75 isn't there" },
        { -2, 0, "the stereo clear of feature 75 isn't after the one of 88" },
    };
    for (int k = 0; k < 5; k++) {
        ft_begin();
        if (cases[k].at >= 0) f_flag[cases[k].at] = cases[k].byte;
        if (cases[k].at == -1) {
            memset(f_flag + 0x6A0, 0xCC, (size_t)aob_len(AOB_FLAG_XESS));
            put(f_flag + 0x800, AOB_FLAG_XESS);
        }
        if (cases[k].at == -2) {
            memset(f_flag + 0x730, 0xCC, (size_t)aob_len(AOB_FLAG_CLEAR_XESS));
            put(f_flag + 0x100, AOB_FLAG_CLEAR_XESS);
        }
        CHECK_MSG(check_xess(f_flag) == 0, "case %d passed", k);
        CHECK_MSG(strstr(ft_log, cases[k].log) != NULL, "case %d: no \"%s\" in:\n%s", k, cases[k].log, ft_log);
        CHECK(strstr(ft_log, "XeSS frame generation stays off in 3D Mode (DLSS and FSR aren't affected)") != NULL);
    }
}

TEST(a_failed_xess_check_leaves_dlss_and_fsr_attached)
{
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    f_flag[0x6A0] = 0x90;
    CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, fake_resolve), 1);
    CHECK_EQ_INT(f_attached, 4);
    CHECK_EQ_INT(fg.xess, 0);
    CHECK(fg.fsrEnabled != NULL);
    CHECK(strstr(ft_log, "hooks attached; DLSS and FSR frame generation stays available") != NULL);
}

TEST(check_sbs_var_wants_odyssey3d_sbsdebug)
{
    ft_begin();
    CHECK_EQ_INT(check_sbs_var((const uint8_t *)f_sbsVar), 1);
    f_sbsVar[VAR_GROUP / 8] = "Rendering";
    CHECK_EQ_INT(check_sbs_var((const uint8_t *)f_sbsVar), 0);
    f_sbsVar[VAR_GROUP / 8] = "Odyssey3D";
    f_sbsVar[VAR_NAME / 8] = NULL;
    CHECK_EQ_INT(check_sbs_var((const uint8_t *)f_sbsVar), 0);
    CHECK(strstr(ft_log, "isn't named [Odyssey3D] SBSDebug") != NULL);
}

TEST(check_framegen_fills_in_the_lookup_and_the_sbsdebug_var)
{
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    uint8_t *t[T_COUNT] = { 0 };
    REQUIRE_EQ_INT(check_handler(fake_resolve, t), 1);
    CHECK_EQ_INT(check_framegen(fake_resolve, t), 1);
    CHECK_EQ_PTR(t[T_FLAG_COMPUTE], f_flag);
    CHECK_EQ_PTR(t[T_DLSSG], f_dlssg);
    CHECK_EQ_PTR((void *)fg.root, f_lookup[0]);
    CHECK_EQ_PTR((void *)fg.group, f_lookup[1]);
    CHECK_EQ_PTR((void *)fg.var, f_lookup[2]);
    CHECK_EQ_PTR(fg.presetsName, &f_cnames[0]);
    CHECK_EQ_PTR(fg.fgName, &f_cnames[1]);
    CHECK_EQ_PTR(fg.sbsVar, f_sbsVar);
    CHECK(fg.tls != 0 && fg.tls != TLS_OUT_OF_INDEXES);
    TlsFree(fg.tls);
}

TEST(attach_range_detaches_what_it_attached_when_one_fails)
{
    ft_begin();
    uint8_t *t[T_COUNT] = { f_setFg, f_o3d, f_dlssg, f_flag };
    f_attachFails = 2;
    CHECK_EQ_INT(attach_range(NULL, &f_hooking, t, T_SET_FG, T_O3D), 0);
    CHECK_EQ_INT(f_detached, 1);
    CHECK(strstr(ft_log, "couldn't hook the 3D Mode handler") != NULL);
    ft_begin();
    t[T_DLSSG] = f_dlssg;
    CHECK_EQ_INT(attach_range(NULL, &f_hooking, t, T_DLSSG, T_FLAG_COMPUTE), 1);
    CHECK_EQ_INT(f_attached, 2);
    CHECK_EQ_PTR((void *)fg.dlssgOrig, f_dlssg);
    CHECK_EQ_PTR((void *)fg.flagOrig, f_flag);
}

TEST(red4ext_resolver_needs_red4ext)
{
    CHECK(red4ext_resolver() == NULL); // no RED4ext.dll in this process
}

// ===================================================================================== the gun

TEST(check_gun_finds_the_plane_override_and_the_camera_build)
{
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    CHECK_EQ_PTR(check_gun(fake_resolve), f_override);
    CHECK_EQ_PTR((void *)fg.cameraBuild, f_buildCode);
    CHECK_EQ_PTR(fg.sbsVar, f_sbsVar);
    CHECK(strstr(ft_log, "gun: plane override checked (rebuild call at +0xD2") != NULL);
}

TEST(the_gun_hook_attaches_on_its_own)
{
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    fg.boolVar = fake_bool_var; // the settings lookups (found by the other checks)
    fg.var = fake_find;
    CHECK_EQ_INT(attach_gun(NULL, &f_hooking, fake_resolve), 1);
    CHECK(fg.planeOrig == (PlaneOverrideFn)(void *)f_override);
    CHECK_EQ_INT(f_override[0], 0xE9);
    CHECK(strstr(ft_log, "gun: hook attached") != NULL);
    CHECK_EQ_INT(f_attached, 2);
    CHECK(fg.sepOrig == (WeaponSepFn)(void *)f_sep);
    CHECK(strstr(ft_log, "gun: weapon separation hooked") != NULL);
}

TEST(the_gun_hook_attaches_without_the_weapon_separation)
{
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    fg.boolVar = fake_bool_var;
    fg.var = fake_find;
    f_sep[0] = 0x90; // a game update changed the getter
    CHECK_EQ_INT(attach_gun(NULL, &f_hooking, fake_resolve), 1);
    CHECK_EQ_INT(f_attached, 1);
    CHECK(fg.sepOrig == NULL);
    CHECK(strstr(ft_log, "gun: the weapon separation getter doesn't match (game update?)") != NULL);
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    fg.boolVar = fake_bool_var;
    fg.var = fake_find;
    f_attachFails = 2;
    CHECK_EQ_INT(attach_gun(NULL, &f_hooking, fake_resolve), 1);
    CHECK(fg.sepOrig == NULL);
    CHECK(strstr(ft_log, "gun: couldn't hook the weapon separation getter") != NULL);
}

TEST(the_gun_hook_attaches_when_the_frame_generation_hooks_cant)
{
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    f_o3d[0] = 0x90;
    CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, fake_resolve), 0);
    CHECK_EQ_INT(attach_gun(NULL, &f_hooking, fake_resolve), 1);
    CHECK_EQ_INT(f_attached, 1);
}

static uintptr_t resolve_without_the_rebuild(uint32_t hash) { return hash == HASH_PLANE_REBUILD ? 0 : fake_resolve(hash); }

TEST(the_gun_hook_isnt_attached_when_the_game_code_changed)
{
    static const char *const logs[] = {
        "didn't resolve into the game exe",
        "the plane override: start doesn't match",
        "the plane override: the rebuild call isn't there",
        "the rebuild call doesn't go to the plane rebuild",
        "the plane override calls the plane rebuild somewhere else too",
        "the plane rebuild: start doesn't match",
        "the eye cameras aren't given the position where expected",
        "the left eye camera isn't rebuilt by the rebuild",
        "the camera build after the position isn't there",
        "the camera build call points outside the exe",
        "the plane rebuild calls the camera build somewhere else too",
        "the loop to the eye cameras isn't there",
        "isn't named [Odyssey3D] SBSDebug",
    };
    for (int k = 0; k < 13; k++) {
        ft_begin();
        memset(&fg, 0, sizeof(fg));
        ResolveFn resolve = fake_resolve;
        switch (k) {
        case 0: resolve = resolve_without_the_rebuild; break;
        case 1: f_override[0] = 0x90; break;
        case 2: f_override[0xCB] = 0x90; break;
        case 3: put_rel32(f_override + 0xCB + aob_len(AOB_OVERRIDE_REBUILD) + 4, f_other); break;
        case 4: f_override[0x60] = 0xE8, put_rel32(f_override + 0x65, f_rebuild); break;
        case 5: f_rebuild[0] = 0x90; break;
        case 6: f_rebuild[0xE2 + 50] = 0x13; break; // [rbx] -> [rbx+...]: not the position
        case 7: put_rel32(f_rebuild + 0xE2 + REBUILD_EYES_CALL + 5, f_other); break;
        case 8: f_rebuild[0x19E + 4] = 0x8B; break; // the position in metres goes elsewhere
        case 9: *(int32_t *)(f_rebuild + 0x19E + aob_len(AOB_REBUILD_BUILD)) = 0x7FFFFFF0; break;
        case 10: f_rebuild[0x60] = 0xE8, put_rel32(f_rebuild + 0x65, f_buildCode); break;
        case 11: f_rebuild[0x201 + 6] = 0x02; break; // tests another flag
        case 12: f_sbsVar[VAR_NAME / 8] = "StereoEnabled"; break;
        }
        CHECK_MSG(attach_gun(NULL, &f_hooking, resolve) == 0, "case %d attached", k);
        CHECK_EQ_INT(f_attached, 0);
        CHECK_MSG(strstr(ft_log, logs[k]) != NULL, "case %d: no \"%s\" in:\n%s", k, logs[k], ft_log);
        CHECK(strstr(ft_log, "gun: not hooked: in 3D the weapon stays flat, as in the game") != NULL);
    }
}

TEST(a_failed_gun_attach_is_logged)
{
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    f_attachFails = 1;
    CHECK_EQ_INT(attach_gun(NULL, &f_hooking, fake_resolve), 0);
    CHECK(strstr(ft_log, "gun: couldn't hook the plane override") != NULL);
}

TEST(the_weapons_eye_cameras_are_put_back_and_built_again)
{
    ft_begin();
    uint8_t want[2][0x400];
    memcpy(want, f_eyes, sizeof(want));
    float override[8] = { 0 };
    CHECK(hook_plane_override(override, f_centre) == 0x5678);
    CHECK_EQ_INT(f_overrideCalls, 1);
    CHECK_EQ_PTR(f_overrideSeen, override);
    CHECK_EQ_PTR(f_cameraSeen, f_centre);
    for (int e = 0; e < 2; e++) {
        CHECK_MSG(memcmp(f_eyes[e] + CAMERA_POSITION, want[e] + CAMERA_POSITION, CAMERA_POSITION_SIZE) == 0, "eye %d", e);
        CHECK_MSG(memcmp(f_eyes[e] + CAMERA_WORLD, want[e] + CAMERA_WORLD, CAMERA_WORLD_SIZE) == 0, "eye %d", e);
    }
    REQUIRE_EQ_INT(f_builds, 2);
    CHECK_EQ_PTR(f_built[0], f_eyes[0]);
    CHECK_EQ_PTR(f_built[1], f_eyes[1]);
    CHECK_EQ_INT(f_builtAt[0][0], 131072 - 4194); // built at its own place, not at the centre
    CHECK_EQ_INT(f_builtAt[1][0], 131072 + 4194);
    CHECK(strstr(ft_log, "gun: first weapon view built with its own eye cameras (right eye 64.0, 0.0, 0.0 mm from the "
                         "left)") != NULL);
}

TEST(eye_cameras_the_rebuild_didnt_move_arent_built_again)
{
    ft_begin();
    f_rebuildMoves = 0; // the plane matched its override: nothing rebuilt
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_overrideCalls, 1);
    CHECK_EQ_INT(f_builds, 0);
    ft_begin();
    place(f_eyes[0], 131072, 262144, 65536); // aiming: the eyes sit at the centre
    place(f_eyes[1], 131072, 262144, 65536);
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 0);
    CHECK(strstr(ft_log, "weapon view") == NULL);
}

TEST(without_sbs_debug_the_weapon_stays_as_the_game_builds_it)
{
    ft_begin();
    set_sbs_debug(0);
    CHECK(hook_plane_override(NULL, f_centre) == 0x5678);
    CHECK_EQ_INT(f_overrideCalls, 1);
    CHECK_EQ_INT(f_builds, 0);
    CHECK(memcmp(f_eyes[0] + CAMERA_POSITION, f_centre + CAMERA_POSITION, CAMERA_POSITION_SIZE) == 0);
    set_sbs_debug(1);
}

TEST(a_camera_without_eye_cameras_is_left_alone)
{
    ft_begin();
    f_centre[CAMERA_FLAGS] = 0;
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 0);
    ft_begin();
    *(uint8_t **)(f_centre + CAMERA_RIGHT_EYE) = NULL;
    f_rebuildMoves = 0; // the fake would follow the NULL
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 0);
    ft_begin();
    CHECK(hook_plane_override(NULL, NULL) == 0x5678);
    CHECK_EQ_INT(f_overrideCalls, 1);
    CHECK_EQ_INT(f_builds, 0);
}

TEST(weapon_views_are_logged_first_and_then_once_a_minute)
{
    ft_begin();
    hook_plane_override(NULL, f_centre);
    fake_cameras();
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 4);
    CHECK(strstr(ft_log, "weapon views built") == NULL);
    fg.gunCount.lastSummary = GetTickCount() - 61000;
    fake_cameras();
    hook_plane_override(NULL, f_centre);
    CHECK(strstr(ft_log, "gun: 3 weapon views built with their own eye cameras (3 in the last minute)") != NULL);
}

// Turns one ray tracing setting on: RT_BOOLS[i], or RT Lighting for RT_LIGHTING.
static void rt_on(int i)
{
    if (i < RT_BOOL_COUNT) f_rtBools[i][BOOL_SETTING_VALUE] = 1;
    else *(int32_t *)(f_rtLighting + SETTING_APPLIED) = 2; // Ultra
}

TEST(each_listed_ray_tracing_setting_keeps_the_weapon_flat)
{
    for (int i = RT_PATH; i <= RT_LIGHTING; i++) {
        const char *name = i < RT_BOOL_COUNT ? RT_BOOLS[i] : "RayTracedLighting";
        ft_begin();
        f_rtBools[RT_MASTER][BOOL_SETTING_VALUE] = 1;
        rt_on(i);
        CHECK(hook_plane_override(NULL, f_centre) == 0x5678);
        CHECK_EQ_INT(f_overrideCalls, 1);
        CHECK_MSG(f_builds == 0, "%s", name);
        CHECK(memcmp(f_eyes[0] + CAMERA_POSITION, f_centre + CAMERA_POSITION, CAMERA_POSITION_SIZE) == 0);
        char want[96];
        snprintf(want, sizeof(want), "gun: ray tracing is on (%s): the weapon stays flat", name);
        CHECK_MSG(strstr(ft_log, want) != NULL, "%s", want);
    }
}

// The gun's log lines when the weapon gets its depth back, and when its eye gap goes to 0.
#define LOG_GUN_DEPTH \
    "gun: no path tracing, RT Lighting or RT shadows: the weapon has depth"
#define LOG_GAP_ZERO \
    "gun: path tracing or RT Lighting or Shadows on (or unknown): the weapon's eye gap is 0 (as Weapon Depth 0)"

TEST(rt_reflections_keep_the_weapons_depth)
{
    // Only the settings seen to break the weapon are checked: Reflections isn't even looked up (the fake lookup fails
    // a test on any name it doesn't know). Local Shadows is (seen to break it).
    int localShadows = 0;
    for (int i = 0; i < RT_BOOL_COUNT; i++) {
        CHECK_MSG(strcmp(RT_BOOLS[i], "RayTracedReflections") != 0, "%s", RT_BOOLS[i]);
        localShadows |= strcmp(RT_BOOLS[i], "RayTracedLocalShadows") == 0;
    }
    CHECK(localShadows);
    ft_begin();
    f_rtBools[RT_MASTER][BOOL_SETTING_VALUE] = 1; // ray tracing on, with only Reflections (not looked at)
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 2);
    CHECK(strstr(ft_log, LOG_GUN_DEPTH) != NULL);
}

TEST(with_the_ray_tracing_switch_off_only_path_tracing_counts)
{
    // The game keeps the other settings' values while its Ray Tracing switch is off (greyed out), and doesn't use them.
    for (int i = RT_PATH + 1; i <= RT_LIGHTING; i++) {
        ft_begin();
        rt_on(i);
        hook_plane_override(NULL, f_centre);
        CHECK_MSG(f_builds == 2, "%s", i < RT_BOOL_COUNT ? RT_BOOLS[i] : "RayTracedLighting");
    }
    ft_begin();
    rt_on(RT_PATH); // path tracing has a switch of its own
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 0);
    CHECK(strstr(ft_log, "gun: ray tracing is on (RayTracedPathTracing)") != NULL);
}

TEST(the_weapon_follows_the_ray_tracing_settings_as_they_change)
{
    ft_begin();
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 2);
    rt_on(RT_PATH); // path tracing turned on in game
    fake_cameras();
    hook_plane_override(NULL, f_centre);
    fake_cameras();
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 2);
    f_rtBools[RT_PATH][BOOL_SETTING_VALUE] = 0; // and off again
    fake_cameras();
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 4);
    CHECK_EQ_INT(f_rtLookups, 0); // the render threads only read what the handler hook found
    const char *on = strstr(ft_log, "gun: ray tracing is on (RayTracedPathTracing)");
    REQUIRE(strstr(ft_log, LOG_GUN_DEPTH) != NULL && on != NULL);
    CHECK(strstr(on, LOG_GUN_DEPTH) != NULL); // off, on, off again
    CHECK(strstr(on + 10, "gun: ray tracing is on") == NULL); // each change logged once
}

TEST(the_ray_tracing_settings_are_found_when_3d_mode_goes_on)
{
    ft_begin();
    memset((void *)fg.rtVars, 0, sizeof(fg.rtVars));
    fg.rtSearched = 0; // not found yet
    hook_plane_override(NULL, f_centre);
    fake_cameras();
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 0);
    CHECK_EQ_INT(f_rtLookups, 0);
    const char *log = strstr(ft_log, "gun: the ray tracing settings weren't found: the weapon stays flat");
    REQUIRE(log != NULL);
    CHECK(strstr(log + 45, "weren't found") == NULL);
    f_o3dSetsFgOff = 0; // the handler took the 3D-off branch: nothing looked up
    menu_switch(0, 0);
    f_o3dSetsFgOff = 1;
    CHECK_EQ_INT(f_rtLookups, 0);
    menu_switch(0, 0); // 3D Mode on at start-up
    CHECK_EQ_INT(f_rtLookups, RT_BOOL_COUNT);
    menu_switch(0, 1); // and in the menu: found once
    CHECK_EQ_INT(f_rtLookups, RT_BOOL_COUNT);
    fake_cameras();
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 2);
    CHECK(strstr(ft_log, LOG_GUN_DEPTH) != NULL);
}

TEST(without_the_games_lookups_the_weapon_stays_flat)
{
    ft_begin();
    memset((void *)fg.rtVars, 0, sizeof(fg.rtVars));
    fg.rtSearched = 0;
    fg.boolVar = NULL; // the screen effects checks failed
    menu_switch(0, 0);
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_rtLookups, 0);
    CHECK_EQ_INT(f_builds, 0);
    ft_begin();
    memset((void *)fg.rtVars, 0, sizeof(fg.rtVars));
    fg.rtSearched = 0;
    fg.var = fake_find_nothing; // RT Lighting not found
    menu_switch(0, 0);
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 0);
    CHECK(strstr(ft_log, "gun: couldn't find the RayTracedLighting setting: the weapon stays flat") != NULL);
    CHECK(fg.rtVars[0] == NULL); // nothing half set
}

TEST(rt_lightings_off_is_found_by_name)
{
    static uint8_t values[4][CSTRING_SIZE];
    static const char *const names[] = { "Psycho", "Off", "Medium", "Ultra" }; // a made-up order
    ft_begin();
    for (int i = 0; i < 4; i++) put_cstring(values[i], names[i]);
    memset((void *)fg.rtVars, 0, sizeof(fg.rtVars));
    fg.rtSearched = 0;
    *(uint8_t **)(f_rtLighting + SETTING_VALUES) = &values[0][0];
    *(uint32_t *)(f_rtLighting + SETTING_VALUES + 0xC) = 4;
    *(int32_t *)(f_rtLighting + SETTING_APPLIED) = 1; // Off
    f_rtBools[RT_MASTER][BOOL_SETTING_VALUE] = 1;
    menu_switch(0, 0);
    CHECK_EQ_INT(fg.rtLightingOff, 1);
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 2);
    *(int32_t *)(f_rtLighting + SETTING_APPLIED) = 0; // Psycho
    fake_cameras();
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 2);
    CHECK(strstr(ft_log, "gun: ray tracing is on (RayTracedLighting)") != NULL);
}

TEST(rt_lighting_without_off_keeps_the_weapon_flat)
{
    ft_begin();
    memset((void *)fg.rtVars, 0, sizeof(fg.rtVars));
    fg.rtSearched = 0;
    *(uint32_t *)(f_rtLighting + SETTING_VALUES + 0xC) = 0; // an empty list
    menu_switch(0, 0);
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 0);
    CHECK(strstr(ft_log, "gun: RayTracedLighting has no Off: the weapon stays flat") != NULL);
    size_t logged = strlen(ft_log);
    menu_switch(0, 0); // the next switch doesn't look (or log) again
    CHECK(strstr(ft_log + logged, "RayTracedLighting") == NULL);
}

TEST(rt_lighting_with_other_values_but_no_off_keeps_the_weapon_flat)
{
    ft_begin();
    memset((void *)fg.rtVars, 0, sizeof(fg.rtVars));
    fg.rtSearched = 0;
    uint8_t *values = *(uint8_t **)(f_rtLighting + SETTING_VALUES);
    put_cstring(values, "None"); // a game update renamed it
    menu_switch(0, 0);
    hook_plane_override(NULL, f_centre);
    CHECK_EQ_INT(f_builds, 0);
    CHECK(strstr(ft_log, "settings: no 'Off' in the list (flags 0x8F, 4 values, 4 read as text)") != NULL);
    CHECK(strstr(ft_log, "gun: RayTracedLighting has no Off: the weapon stays flat") != NULL);
}

TEST(without_eye_cameras_or_sbs_debug_the_settings_arent_read)
{
    ft_begin();
    memset((void *)fg.rtVars, 0, sizeof(fg.rtVars));
    fg.rtSearched = 0;
    set_sbs_debug(0);
    hook_plane_override(NULL, f_centre);
    set_sbs_debug(1);
    f_centre[CAMERA_FLAGS] = 0;
    hook_plane_override(NULL, f_centre);
    CHECK(strstr(ft_log, "gun: the ray tracing settings") == NULL);
}


TEST(with_ray_tracing_on_the_weapon_separation_is_0)
{
    ft_begin();
    fg.sepOrig = fake_weapon_sep;
    CHECK(hook_weapon_sep(NULL) == 0.017f); // ray tracing off: the game's value
    f_rtBools[RT_MASTER][BOOL_SETTING_VALUE] = 1;
    rt_on(2); // Sun Shadows
    CHECK(hook_weapon_sep(NULL) == 0.0f);
    CHECK(hook_weapon_sep(NULL) == 0.0f);
    set_sbs_debug(0); // a real 3D monitor: the game's rule
    CHECK(hook_weapon_sep(NULL) == 0.017f);
    set_sbs_debug(1);
    f_rtBools[2][BOOL_SETTING_VALUE] = 0;
    CHECK(hook_weapon_sep(NULL) == 0.017f);
    const char *on = strstr(ft_log, LOG_GAP_ZERO);
    REQUIRE(on != NULL);
    CHECK(strstr(on + 10, "gun: path tracing or RT Lighting or Shadows on") == NULL); // each change logged once
    CHECK(strstr(ft_log, "gun: the weapon's eye gap follows Weapon Depth again") != NULL);
    memset((void *)fg.rtVars, 0, sizeof(fg.rtVars));
    fg.rtSearched = 0; // settings not found: the weapon is flat, so at screen depth
    CHECK(hook_weapon_sep(NULL) == 0.0f);
}

TEST(the_eye_gap_is_logged_as_a_distance_when_it_changes)
{
    ft_begin();
    fg.gapTenths = 0; // nothing logged yet
    EyePlaces kept;
    memset(&kept, 0, sizeof(kept));
    float *l = (float *)kept.world[0], *r = (float *)kept.world[1];
    r[0] = 0.017f; // 17 mm along x
    log_gun_gap(&kept);
    const char *after = strstr(ft_log, "gun: the weapon's eye cameras are now 17.0 mm apart");
    REQUIRE(after != NULL);
    after += strlen("gun: the weapon's eye cameras are now 17.0 mm apart");
    r[0] = 0.0f, r[1] = 0.017f; // turned 90 degrees: the same gap, nothing logged
    log_gun_gap(&kept);
    l[0] = 0.0001f; // 0.1 mm: under the threshold
    log_gun_gap(&kept);
    CHECK(strstr(after, "eye cameras are now") == NULL);
    r[1] = 0.005f; // Weapon Depth changed, but within a second of the last line
    log_gun_gap(&kept);
    CHECK(strstr(after, "eye cameras are now") == NULL);
    fg.gapTick = GetTickCount() - 1500; // a second and a half later
    log_gun_gap(&kept);
    CHECK(strstr(after, "gun: the weapon's eye cameras are now 5.0 mm apart") != NULL);
}

TEST(the_weapon_separation_thunk_returns_the_hooks_answer)
{
    ft_begin();
    fg.sepOrig = fake_weapon_sep;
    CHECK(bd3d_thunk_weapon_sep(NULL) == 0.017f);
    rt_on(RT_PATH);
    CHECK(bd3d_thunk_weapon_sep(NULL) == 0.0f);
}

// ===================================================================================== screen effects

TEST(cname_is_the_engines_fnv1a_hash)
{
    // The CNames SetFrameGeneration looks up, and one seen as an immediate in the 2.5 exe.
    CHECK(cname("/graphics/presets") == 0x1AF15AB3D7A612E0ull);
    CHECK(cname("FrameGeneration") == 0x1ACE52F474ADA088ull);
    CHECK(cname("MotionBlur") == 0x8F10615A2F8D4192ull);
}

TEST(check_effects_finds_the_games_lookup_and_setters)
{
    ft_begin();
    memset(&fg, 0, sizeof(fg));
    CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, fake_resolve), 1);
    CHECK(fg.boolVar == (BoolVarFn)(void *)f_boolVarCode);
    CHECK(fg.boolSet == (BoolSetFn)(void *)f_boolSetCode);
    CHECK(fg.listSet == (ListSetFn)(void *)f_listSetCode);
    CHECK_EQ_PTR(fg.gameOff[0], &f_effectSlots[0]);
    CHECK_EQ_PTR(fg.gameOff[1], &f_effectSlots[2]);
    CHECK(strstr(ft_log, "effects: checked (the handler's lookup at +0xDF, its effects off at +0x2D2)") != NULL);
}

TEST(the_effects_off_isnt_set_up_when_the_game_code_changed)
{
    static const char *const logs[] = {
        "the settings it turns off aren't looked up where expected",
        "its own effects off isn't there",
        "its effects off comes before the lookup",
        "the list setter call isn't there",
        "its two settings aren't looked up the same way",
        "its two settings aren't turned off the same way",
    };
    for (int k = 0; k < 6; k++) {
        ft_begin();
        memset(&fg, 0, sizeof(fg));
        switch (k) {
        case 0: f_o3d[0xDF + 7] = 0x90; break;
        case 1: f_o3d[0x2D2 + 14] = 0x90; break;
        case 2:
            memset(f_o3d + 0xDF, 0xCC, (size_t)aob_len(AOB_O3D_EFFECT_VARS) + 4);
            put(f_o3d + 0x400, AOB_O3D_EFFECT_VARS);
            break;
        case 3: f_setFg[0x90 + 7] = 0x90; break;
        case 4: put_rel32(f_o3d + 0xDF + aob_len(AOB_O3D_EFFECT_VARS) + 4, f_other); break;
        case 5: put_rel32(f_o3d + 0x2D2 + aob_len(AOB_O3D_EFFECTS_OFF) + 4, f_other); break;
        }
        CHECK_MSG(attach_hooks(NULL, &f_hooking, fake_resolve) == 1, "case %d: frame generation should still attach", k);
        CHECK_MSG(fg.boolVar == NULL && fg.boolSet == NULL && fg.listSet == NULL, "case %d set up", k);
        CHECK_MSG(strstr(ft_log, logs[k]) != NULL, "case %d: no \"%s\" in:\n%s", k, logs[k], ft_log);
        CHECK(strstr(ft_log, "effects: switching 3D Mode on turns off only what the game turns off") != NULL);
    }
}


TEST(a_menu_switch_to_3d_turns_the_effects_off)
{
    ft_begin();
    menu_switch(0, 1);
    REQUIRE_EQ_INT(f_boolSets, EFFECT_COUNT + HUD_REMOVAL_COUNT);
    for (int i = 0; i < HUD_REMOVAL_COUNT; i++) {
        CHECK_MSG(f_boolSetVar[EFFECT_COUNT + i] == f_hud[i], "%s", HUD_REMOVALS[i]);
        CHECK_EQ_INT(f_boolSetValue[EFFECT_COUNT + i], 1); // removed
        CHECK_EQ_INT(f_boolSetMode[EFFECT_COUNT + i], SET_FROM_MENU);
    }
    CHECK(strstr(ft_log, "effects: PostProcessingReduction on (the HUD's visual effects removed") != NULL);
    CHECK(strstr(ft_log, "effects: LensDistortionOverride on (the HUD's lens distortion removed") != NULL);
    for (int i = 0; i < EFFECT_COUNT; i++) {
        CHECK_MSG(f_boolSetVar[i] == f_bools[i], "%s", EFFECTS[i]);
        CHECK_EQ_INT(f_boolSetValue[i], 0);
        CHECK_EQ_INT(f_boolSetMode[i], SET_FROM_MENU);
    }
    CHECK_EQ_INT(f_listSets, 1);
    CHECK_EQ_PTR(f_listSetVar, f_blur);
    CHECK_EQ_INT(f_listSetIndex, 0); // Off
    CHECK_EQ_INT(f_listSetMode, SET_FROM_MENU);
    CHECK(strstr(ft_log, "the game turned off ChromaticAberration and FilmGrain, turning off the rest") != NULL);
    CHECK(strstr(ft_log, "effects: 6 of 6 off (ChromaticAberration, FilmGrain, DepthOfField, LensFlares, Vignette, "
                         "MotionBlur; each can be turned back on)") != NULL);
    CHECK(strstr(ft_log, "isn't /graphics/basic") == NULL);
}

TEST(the_effects_use_the_group_the_game_uses)
{
    ft_begin();
    f_effectSlots[1] = cname("/graphics/other"); // a game update moved them
    menu_switch(0, 1);
    CHECK(strstr(ft_log, "effects: the game's group for its two isn't /graphics/basic") != NULL);
    CHECK_EQ_INT(f_boolSets, HUD_REMOVAL_COUNT); // the fake has the effects in /graphics/basic only
    CHECK_EQ_INT(f_listSets, 0);
    CHECK(strstr(ft_log, "effects: 0 of 6 off (none; each can be turned back on)") != NULL);
}

TEST(the_effects_stay_as_they_are_otherwise)
{
    ft_begin();
    menu_switch(0, 0); // start-up or a display change
    menu_switch(1, 1); // the game's own condition wants [0] clear too
    hook_o3d_changed(NULL);
    f_o3dSetsFgOff = 0; // the handler took the 3D-off branch
    menu_switch(0, 1);
    f_o3dSetsFgOff = 1;
    set_sbs_debug(0); // a real 3D monitor: the game's rule
    menu_switch(0, 1);
    set_sbs_debug(1);
    fg.framegen = 0; // the frame generation checks failed (no SBSDebug var)
    menu_switch(0, 1);
    fg.framegen = 1;
    fg.boolVar = NULL; // the effects checks failed
    menu_switch(0, 1);
    CHECK_EQ_INT(f_boolSets, 0);
    CHECK_EQ_INT(f_listSets, 0);
    CHECK(strstr(ft_log, "effects:") == NULL);
}

TEST(a_setting_that_isnt_found_is_left_as_it_is)
{
    ft_begin();
    f_missingBool = 4; // Vignette
    *(uint32_t *)(f_blur + SETTING_VALUES + 0xC) = 0; // Motion Blur's list empty
    menu_switch(0, 1);
    CHECK_EQ_INT(f_boolSets, EFFECT_COUNT - 1 + HUD_REMOVAL_COUNT);
    CHECK_EQ_INT(f_listSets, 0);
    CHECK(strstr(ft_log, "effects: couldn't find the Vignette setting, left as it is") != NULL);
    CHECK(strstr(ft_log, "effects: couldn't find the Motion Blur setting or its Off, left as it is") != NULL);
    CHECK(strstr(ft_log, "settings: no 'Off' in the list (flags 0x8F, 0 values, 0 read as text)") != NULL);
    CHECK(strstr(ft_log, "effects: 4 of 6 off (ChromaticAberration, FilmGrain, DepthOfField, LensFlares; each can be "
                         "turned back on)") != NULL);
}

TEST(motion_blurs_off_is_found_by_name)
{
    ft_begin();
    static const char *const names[] = { "High", "Low", "Off" }; // another order: the index comes from the name
    for (int i = 0; i < 3; i++) put_cstring(f_blurValues[i], names[i]);
    menu_switch(0, 1);
    CHECK_EQ_INT(f_listSets, 1);
    CHECK_EQ_INT(f_listSetIndex, 2);
}

TEST(a_list_whose_values_arent_text_is_left_as_it_is)
{
    ft_begin();
    static uint8_t notText[3][CSTRING_SIZE];
    memset(notText, 0, sizeof(notText));
    for (int i = 0; i < 3; i++) {
        *(uint64_t *)notText[i] = 0x8F10615A2F8D4192ull + (uint64_t)i; // CName-like values
        *(uint32_t *)(notText[i] + CSTRING_LEN_FLAGS) = 0xFFFFFFFFu;   // no sensible length
    }
    *(uint8_t **)(f_blur + SETTING_VALUES) = &notText[0][0];
    menu_switch(0, 1);
    CHECK_EQ_INT(f_listSets, 0);
    CHECK(strstr(ft_log, "settings: no 'Off' in the list (flags 0x8F, 3 values, 0 read as text; the first one's "
                         "bytes: 92 41 8D 2F 5A 61 10 8F)") != NULL);
    *(uint8_t **)(f_blur + SETTING_VALUES) = NULL; // or no values at all
    menu_switch(0, 1);
    CHECK_EQ_INT(f_listSets, 0);
    CHECK(strstr(ft_log, "3 values, 0 read as text; the first one's bytes: unreadable)") != NULL);
}

TEST(unknown_settings_the_game_turns_off_are_named_so)
{
    ft_begin();
    f_effectSlots[2] = 0x1234;
    menu_switch(0, 1);
    CHECK(strstr(ft_log, "the game turned off ChromaticAberration and a setting this doesn't know") != NULL);
    CHECK_EQ_INT(f_boolSets, EFFECT_COUNT + HUD_REMOVAL_COUNT);
}

int main(int argc, char **argv) { return tt_main(argc, argv); }
