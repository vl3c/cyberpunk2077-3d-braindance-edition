// White-box tests for the frame generation hooks: this file #includes src/braindance3d_framegen.c so it can run
// the byte-pattern checks against a fake copy of the game's code and drive the hooks with fake originals.
// Whether DLSS frame generation really runs in 3D is only tested in game.
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
static uint8_t f_o3d[O3D_SIZE], f_setFg[64], f_fgMode[16], f_flag[FLAG_COMPUTE_SIZE], f_dlssg[16], f_lookup[3][16];
static uint64_t f_cnames[2];
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

    put(f_setFg, AOB_SET_FG);
    put_rel32(f_setFg + SET_FG_ROOT + 5, f_lookup[0]);
    put_rel32(f_setFg + SET_FG_PRESETS + 7, &f_cnames[0]);
    put_rel32(f_setFg + SET_FG_GROUP + 5, f_lookup[1]);
    put_rel32(f_setFg + SET_FG_NAME + 7, &f_cnames[1]);
    put_rel32(f_setFg + SET_FG_VAR + 5, f_lookup[2]);

    put(f_flag, AOB_FLAG_COMPUTE);
    put(f_flag + 0x5BC, AOB_FLAG_DLSSG);
    put_rel32(f_flag + 0x5BC + FLAG_DLSSG_CALL + 5, f_dlssg);
    put(f_flag + 0x6F0, AOB_FLAG_CLEAR);

    f_sbsVar[VAR_NAME / 8] = "SBSDebug";
    f_sbsVar[VAR_GROUP / 8] = "Odyssey3D";
    ((uint8_t *)f_sbsVar)[VAR_VALUE] = 1;
}

static uintptr_t fake_resolve(uint32_t hash)
{
    switch (hash) {
    case HASH_O3D_CHANGED: return (uintptr_t)f_o3d;
    case HASH_SET_FG: return (uintptr_t)f_setFg;
    case HASH_FG_MODE: return (uintptr_t)f_fgMode;
    case HASH_FLAG_COMPUTE: return (uintptr_t)f_flag;
    case HASH_DLSSG_ENABLED: return (uintptr_t)f_dlssg;
    case HASH_VAR_SBSDEBUG: return (uintptr_t)f_sbsVar;
    }
    return 0;
}

static uintptr_t resolve_nothing(uint32_t hash) { return 0; }

static bool fake_attach(void *handle, void *target, void *detour, void **original)
{
    if (++f_attached == f_attachFails) return false;
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
static uint32_t f_mode = FG_MODE_DLSS;
static int f_setFgCalls, f_setFgIndex, f_o3dSetsFgOff, f_o3dGreys, f_eligible, f_dlssgOn, f_dlssgCalls;
static uint8_t f_var[0x50];

static uint8_t orig_set_fg(int32_t index, uint8_t mode)
{
    f_setFgCalls++;
    f_setFgIndex = index;
    return 1;
}

// Does what the game's handler does with 3D Mode on: Frame Generation Off, then the greyed flag.
static void orig_o3d(uint8_t *event)
{
    if (f_o3dSetsFgOff) hook_set_fg(0, 1);
    if (f_o3dGreys) *(uint32_t *)(f_var + SETTING_FLAGS) |= FLAG_GREYED;
}

static uint32_t fake_fg_mode(void) { return f_mode; }
static void *fake_root(void) { return f_lookup; }
static void *fake_find(void *parent, uint64_t cname) { return cname == f_cnames[1] ? (void *)f_var : (void *)f_lookup; }

static uint8_t fake_dlssg(uint8_t *sl)
{
    f_dlssgCalls++;
    return (uint8_t)f_dlssgOn;
}

// Like the game's: asks IsDlssgEnabled (through the hook, as it would be patched) only for an eligible view.
static uint64_t f_flagsAfterOrig[2];
static void orig_flag_compute(uint8_t *renderer, uint64_t *flags, void *frame, void *settings)
{
    if (f_eligible) hook_dlssg_enabled(NULL);
    flags[0] = f_flagsAfterOrig[0];
    flags[1] = f_flagsAfterOrig[1];
}

static void ft_begin(void)
{
    static DWORD tls = TLS_OUT_OF_INDEXES;
    if (tls == TLS_OUT_OF_INDEXES) tls = TlsAlloc();
    memset(&fg, 0, sizeof(fg));
    fg.tls = tls;
    ft_log[0] = 0;
    fake_game();
    f_attachFails = f_attached = f_detached = 0;
    f_mode = FG_MODE_DLSS;
    f_setFgCalls = f_setFgIndex = 0;
    f_o3dSetsFgOff = f_o3dGreys = 1;
    f_eligible = f_dlssgOn = 1;
    f_dlssgCalls = 0;
    memset(f_var, 0, sizeof(f_var));
    f_cnames[0] = 0x1AF15AB3D7A612E0ull;
    f_cnames[1] = 0x1ACE52F474ADA088ull;
    fg.setFgOrig = orig_set_fg;
    fg.o3dOrig = orig_o3d;
    fg.flagOrig = orig_flag_compute;
    fg.fgMode = fake_fg_mode;
    fg.dlssgOrig = fake_dlssg;
    fg.root = fake_root;
    fg.group = fake_find;
    fg.var = fake_find;
    fg.presetsName = &f_cnames[0];
    fg.fgName = &f_cnames[1];
    fg.sbsVar = (const uint8_t *)f_sbsVar;
    f_flagsAfterOrig[0] = 0;
    f_flagsAfterOrig[1] = FEATURE_BIT(FEATURE_STEREO);
}

static void set_sbs_debug(int on) { ((uint8_t *)f_sbsVar)[VAR_VALUE] = (uint8_t)on; }

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
    CHECK(fg.setFgOrig == (SetFgFn)(void *)f_setFg);
    CHECK(fg.o3dOrig == (O3dChangedFn)(void *)f_o3d);
    CHECK(fg.flagOrig == (FlagComputeFn)(void *)f_flag);
    CHECK(fg.root == (RootFn)(void *)f_lookup[0]);
    CHECK(fg.group == (FindFn)(void *)f_lookup[1]);
    CHECK(fg.var == (FindFn)(void *)f_lookup[2]);
    CHECK(fg.presetsName == &f_cnames[0]);
    CHECK(fg.fgName == &f_cnames[1]);
    CHECK(fg.dlssgOrig == (DlssgEnabledFn)(void *)f_dlssg);
    CHECK(fg.fgMode == (FgModeFn)(void *)f_fgMode);
    CHECK(strstr(ft_log, "all checks passed (SBSDebug is on right now)") != NULL);
}

// Each broken check: nothing is attached and the log names it.
static void expect_no_hooks(const char *why)
{
    memset(&fg, 0, sizeof(fg));
    ft_log[0] = 0;
    CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, fake_resolve), 0);
    CHECK_EQ_INT(f_attached, 0);
    CHECK_MSG(strstr(ft_log, why) != NULL, "log should say '%s', got: %s", why, ft_log);
    CHECK(strstr(ft_log, "nothing attached") != NULL);
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
    put_rel32(f_o3d + 0x188 + aob_len(AOB_O3D_FG_OFF) + 4, f_fgMode);
    expect_no_hooks("doesn't go to SetFrameGeneration");
    ft_begin();
    f_o3d[0x2FE + 7] = 0x0C; // bts eax,0Ch
    expect_no_hooks("the greyed flag isn't set where expected");
}

TEST(nothing_attaches_when_set_frame_generation_changed)
{
    ft_begin();
    f_setFg[SET_FG_PRESETS] = 0x4C;
    expect_no_hooks("SetFrameGeneration: start doesn't match");
    ft_begin();
    put_rel32(f_setFg + SET_FG_GROUP + 5, (uint8_t *)GetModuleHandleW(NULL) - 0x1000);
    expect_no_hooks("its settings lookup points outside the exe");
}

TEST(nothing_attaches_when_flag_compute_changed)
{
    ft_begin();
    f_flag[1] = 0;
    expect_no_hooks("FlagCompute: start doesn't match");
    ft_begin();
    f_flag[0x5BC + 3] = 0x60; // another renderer offset
    expect_no_hooks("the DLSS-G check isn't there");
    ft_begin();
    put_rel32(f_flag + 0x5BC + FLAG_DLSSG_CALL + 5, f_fgMode);
    expect_no_hooks("doesn't call IsDlssgEnabled");
    ft_begin();
    f_flag[0x6F0 + 14] = 0x59; // feature 89
    expect_no_hooks("the stereo clear of feature 88 isn't there");
    ft_begin();
    f_flag[0x900] = 0xE8; // a second call to IsDlssgEnabled: its answer wouldn't be the DLSS-G decision
    put_rel32(f_flag + 0x905, f_dlssg);
    expect_no_hooks("IsDlssgEnabled is called somewhere else in it too");
}

TEST(nothing_attaches_when_the_sbsdebug_var_is_another_var)
{
    ft_begin();
    f_sbsVar[VAR_NAME / 8] = "StereoEnabled";
    expect_no_hooks("isn't named [Odyssey3D] SBSDebug");
    ft_begin();
    f_sbsVar[VAR_GROUP / 8] = NULL;
    expect_no_hooks("isn't named [Odyssey3D] SBSDebug");
}

TEST(a_failed_attach_detaches_the_hooks_already_attached)
{
    for (int failAt = 1; failAt <= T_COUNT; failAt++) {
        ft_begin();
        f_attachFails = failAt;
        CHECK_EQ_INT(attach_hooks(NULL, &f_hooking, fake_resolve), 0);
        CHECK_EQ_INT(f_detached, failAt - 1);
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
    CHECK(strstr(ft_log, "kept DLSS Frame Generation") != NULL);
}

TEST(handler_keeps_a_greyed_flag_that_was_already_there)
{
    ft_begin();
    *(uint32_t *)(f_var + SETTING_FLAGS) = FLAG_GREYED | 0x5;
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS), FLAG_GREYED | 0x5);
}

TEST(handler_without_sbs_debug_does_what_the_game_does)
{
    ft_begin();
    set_sbs_debug(0);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK_EQ_INT(f_setFgIndex, 0);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, FLAG_GREYED);
}

TEST(handler_still_turns_off_fsr_and_xess_frame_generation)
{
    ft_begin();
    f_mode = 2;
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, FLAG_GREYED);
    CHECK(strstr(ft_log, "isn't DLSS") != NULL);
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
}

TEST(set_frame_generation_outside_the_handler_passes_through)
{
    ft_begin();
    CHECK_EQ_INT(hook_set_fg(0, 3), 1); // graphics preset or reset to defaults
    CHECK_EQ_INT(f_setFgCalls, 1);
    fg.handlerThread = GetCurrentThreadId();
    hook_set_fg(1, 1); // not "Off"
    CHECK_EQ_INT(f_setFgCalls, 2);
    CHECK_EQ_INT(fg.kept, 0);
    fg.handlerThread = GetCurrentThreadId() + 1; // the handler is running on another thread
    hook_set_fg(0, 1);
    CHECK_EQ_INT(f_setFgCalls, 3);
}

// ===================================================================================== FlagCompute

static uint64_t run_flags(void)
{
    uint64_t flags[2] = { 0, 0 };
    hook_flag_compute(NULL, flags, NULL, NULL);
    return flags[1];
}

TEST(stereo_frames_get_dlssg_back_when_the_game_would_run_it)
{
    ft_begin();
    CHECK(run_flags() == (FEATURE_BIT(FEATURE_STEREO) | FEATURE_BIT(FEATURE_DLSSG)));
    CHECK_EQ_INT((int)fg.restored, 1);
    CHECK(strstr(ft_log, "first one") != NULL);
    run_flags();
    CHECK_EQ_INT((int)fg.restored, 2);
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
    CHECK_EQ_INT((int)fg.restored, 0);
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
    *(uint64_t *)arg = run_flags();
    return 0;
}

TEST(each_thread_gets_its_own_dlssg_decision)
{
    ft_begin();
    // An eligible view on a worker thread doesn't make a view on this thread eligible.
    uint64_t other = 0;
    HANDLE t = CreateThread(NULL, 0, eligible_view_thread, &other, 0, NULL);
    REQUIRE(t != NULL);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(other == (FEATURE_BIT(FEATURE_STEREO) | FEATURE_BIT(FEATURE_DLSSG)));
    f_eligible = 0;
    CHECK(run_flags() == FEATURE_BIT(FEATURE_STEREO));
}

int main(int argc, char **argv) { return tt_main(argc, argv); }
