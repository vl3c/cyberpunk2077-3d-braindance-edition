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
static uint8_t f_o3d[O3D_SIZE], f_setFg[64], f_other[16], f_flag[FLAG_COMPUTE_SIZE], f_dlssg[16], f_lookup[3][16];
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
    case HASH_FLAG_COMPUTE: return (uintptr_t)f_flag;
    case HASH_DLSSG_ENABLED: return (uintptr_t)f_dlssg;
    case HASH_VAR_SBSDEBUG: return (uintptr_t)f_sbsVar;
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

static void *fake_root(void) { return f_lookup; }
static void *fake_find(void *parent, uint64_t cname) { return cname == f_cnames[1] ? (void *)f_var : (void *)f_lookup; }
static void *fake_find_nothing(void *parent, uint64_t cname) { return NULL; }

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
    CHECK(strstr(ft_log, "hooks attached; DLSS frame generation stays available") != NULL);
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
    // Saved as FSR3: turned Off and greyed at start-up. The next run sees Off and makes it choosable again.
    ft_begin();
    set_fg_value("FSR3", 1);
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

TEST(handler_does_what_the_game_does_on_a_pc_without_dlss_frame_generation)
{
    // An AMD, Intel or RTX 20/30 PC: the list has no DLSS, and nothing it offers runs in stereo.
    ft_begin();
    set_fg_value("Off", 1);
    put_cstring(f_values[1], "FSR4");
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, FLAG_GREYED);
    CHECK(strstr(ft_log, "Frame Generation is Off (the game offers no DLSS frame generation") != NULL);
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

TEST(keep_in_3d_allows_only_off_and_dlss)
{
    ft_begin();
    set_fg_value("DLSS", 1);
    CHECK(keep_in_3d(f_var, "DLSS", 1));
    CHECK(keep_in_3d(f_var, "Off", 1));
    CHECK(keep_in_3d(f_var, "", 0)); // unreadable counts as Off
    CHECK(!keep_in_3d(f_var, "FSR3", 1));
    CHECK(!keep_in_3d(f_var, "XESS", 1));
    CHECK(!keep_in_3d(f_var, "DLSS2", 1));
    CHECK(offers_dlss(f_var));
    put_cstring(f_values[1], "FSR4");
    CHECK(!offers_dlss(f_var));
    *(uint32_t *)(f_var + SETTING_VALUES + 0xC) = 17; // a size this can't trust
    put_cstring(f_values[1], "DLSS");
    CHECK(!offers_dlss(f_var));
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

TEST(handler_still_turns_off_fsr_and_xess_frame_generation)
{
    ft_begin();
    set_fg_value("FSR3", 1);
    hook_o3d_changed(NULL);
    CHECK_EQ_INT(f_setFgCalls, 1);
    CHECK_EQ_INT(*(uint32_t *)(f_var + SETTING_FLAGS) & FLAG_GREYED, FLAG_GREYED);
    CHECK(strstr(ft_log, "Frame Generation is FSR3, which can't run in 3D") != NULL);
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
                       (void *)bd3d_thunk_flag_compute };
    for (int i = 0; i < 4; i++) {
        DWORD64 base = 0;
        CHECK_MSG(RtlLookupFunctionEntry((DWORD64)(uintptr_t)thunks[i], &base, NULL) != NULL, "thunk %d", i);
    }
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

// Returns rax.
static uint64_t check_thunk_keeps_registers(void *thunk, uint64_t rdxValue)
{
    uint64_t out[13] = { 0 };
    ft_call_thunk(thunk, out, rdxValue);
    uint64_t want[13] = { 0x1111111111111111ull, rdxValue, 0x3333333333333333ull, 0x4444444444444444ull,
                          0x5555555555555555ull, 0x6666666666666666ull, 0, 0x7777777777777777ull,
                          0x8888888888888888ull, 0x9999999999999999ull, 0xAAAAAAAAAAAAAAAAull,
                          0xBBBBBBBBBBBBBBBBull, 0xCCCCCCCCCCCCCCCCull };
    for (int i = 0; i < 13; i++)
        if (i != 6) CHECK_MSG(out[i] == want[i], "register %d changed to %llx", i, (unsigned long long)out[i]);
    return out[6];
}

TEST(thunks_keep_the_callers_registers_and_return_the_result)
{
    ft_begin();
    // IsDlssgEnabled: FlagCompute writes [rcx+0x1F2] right after calling it (the second in-game test's crash).
    f_dlssgOn = 1;
    CHECK_EQ_INT((int)(check_thunk_keeps_registers((void *)bd3d_thunk_dlssg_enabled, 0x2222222222222222ull) & 0xFF), 1);
    CHECK_EQ_INT(f_dlssgCalls, 1);
    // FlagCompute: its caller reads the flags through the returned pointer (the first in-game test's crash).
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

TEST(offers_dlss_looks_through_the_built_list)
{
    ft_begin();
    set_fg_value("Off", 1);
    CHECK_EQ_INT(offers_dlss(f_var), 1);
    put_cstring(f_values[1], "FSR4"); // a PC without DLSS frame generation
    CHECK_EQ_INT(offers_dlss(f_var), 0);
    set_fg_value("Off", -1); // before the list is built it can't tell
    CHECK_EQ_INT(offers_dlss(f_var), 1);
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

TEST(keep_frame_generation_keeps_off_or_dlss_while_sbsdebug_is_on)
{
    ft_begin();
    CHECK_EQ_INT(keep_frame_generation(), 1);
    CHECK_EQ_PTR(fg.keptFlags, f_var + SETTING_FLAGS);
    struct { int framegen, sbs, found; const char *value; const char *log; } cases[] = {
        { 0, 1, 1, "DLSS", NULL },
        { 1, 0, 1, "DLSS", NULL },
        { 1, 1, 0, "DLSS", "couldn't find the Frame Generation setting" },
        { 1, 1, 1, "FSR3", "Frame Generation is FSR3, which can't run in 3D" },
    };
    for (int k = 0; k < 4; k++) {
        ft_begin();
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

TEST(restored_frames_are_logged_first_and_then_once_a_minute)
{
    ft_begin();
    count_restored();
    CHECK(strstr(ft_log, "DLSS frame generation running on a 3D frame (first one)") != NULL);
    count_restored();
    CHECK(strstr(ft_log, "frames with DLSS frame generation in 3D") == NULL);
    fg.lastSummary = GetTickCount() - 61000;
    count_restored();
    CHECK(strstr(ft_log, "3 frames with DLSS frame generation in 3D (3 in the last minute)") != NULL);
    CHECK_EQ_INT((int)fg.lastRestored, 3);
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

int main(int argc, char **argv) { return tt_main(argc, argv); }
