// White-box tests for the menu squeeze: this file #includes src/braindance3d_menus.c so it can call its
// functions and read its state. The game's side is faked: its device data (a buffer at the real offsets),
// its swap chain (a COM object whose Present slots get patched), GpuApi::Present and RED4ext's hooking.
// The D3D12 squeeze runs on a WARP device where Windows has one, and is skipped where it doesn't (Wine).
//
// Run: test_menus.exe [--dll path\to\Odyssey3DSDK.dll] [--list] [name filters...]

#include "../../src/braindance3d_menus.c"

#define TT_IMPLEMENTATION
#include "tinytest.h"
#include "testutil.h"

// ===================================================================================== state reset

static LONG64 fake_device_calls(int slot) { return slot >= 0 ? 0 : -1; }

// Puts the plugin's globals back to a freshly loaded auto mode, with the stand-in's counters "found".
static void mt_begin(void)
{
    static int once;
    if (g.log) fclose(g.log);
    if (once) DeleteCriticalSection(&g.logLock);
    memset(&g, 0, sizeof(g));
    InitializeCriticalSection(&g.logLock);
    once = 1;
    g.mode = MODE_AUTO;
    g.deviceCalls = fake_device_calls;
    g.stereo = STEREO_UNKNOWN;
}

// Frame patterns measured on the PC (2.5, 2026-10-29): what the game asks the stand-in per frame.
static void stereo_frame(void) { note_frame_calls(2, 2); }    // 3D gameplay or title
static void menu_frame(void) { note_frame_calls(0, 0); }      // inventory, ESC, loading
static void map_frame(void) { note_frame_calls(0, 2); }       // the map in 3D
static void flat_2d_frame(void) { note_frame_calls(0, 1); }   // 2D gameplay
static void frames(void (*f)(void), int n) { for (int i = 0; i < n; i++) f(); }

// ===================================================================================== the log

static wchar_t m_logPath[TU_PATH];

static void close_log(void *unused)
{
    if (g.log) fclose(g.log);
    g.log = NULL;
}

// Sends the plugin's log to a temp file for the rest of the test.
static void log_to_file(void)
{
    close_log(NULL);
    wchar_t *dir = tu_tempdir(L"log");
    tu_join(m_logPath, TU_PATH, dir, L"hooks.log");
    g.log = _wfopen(m_logPath, L"w");
    REQUIRE(g.log != NULL);
    tt_defer(close_log, NULL);
}

// What the log holds so far (freed after the test).
static const char *log_text(void)
{
    fflush(g.log);
    char *text = tu_read_file(m_logPath, NULL);
    REQUIRE(text != NULL);
    tt_defer(free, text);
    return text;
}

static int log_has(const char *what) { return strstr(log_text(), what) != NULL; }

// ===================================================================================== the game, faked

// The game's SDeviceData, with the swap chain table and the direct queue at the real offsets.
static uint8_t *m_dd;
static uintptr_t m_ddVar; // the game's SDeviceData* variable

static void fake_device_data(void)
{
    if (!m_dd) m_dd = VirtualAlloc(NULL, DD_DIRECT_QUEUE + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    REQUIRE(m_dd != NULL);
    memset(m_dd + DD_SWAPCHAINS, 0, DD_SWAPCHAIN_COUNT * DD_SWAPCHAIN_STRIDE);
    *(void **)(m_dd + DD_DIRECT_QUEUE) = NULL;
    m_ddVar = (uintptr_t)m_dd;
    g.deviceData = (uintptr_t)&m_ddVar;
}

static void set_swap_chain_entry(int index, void *sc) { *(void **)(m_dd + DD_SWAPCHAINS + index * DD_SWAPCHAIN_STRIDE) = sc; }
static void set_direct_queue(void *q) { *(void **)(m_dd + DD_DIRECT_QUEUE) = q; }

// A swap chain: answers IDXGISwapChain and IDXGISwapChain1 with itself, and IUnknown with its identity
// (another object for a wrapper). Its vtable is shared, like DXGI's, and patched by the tests.
typedef struct FakeSc FakeSc;
struct FakeSc {
    void **lpVtbl;
    LONG refs;
    FakeSc *identity;
    int refuse; // 1: no IDXGISwapChain, 2: no IDXGISwapChain1
    int presents, present1s, sc3Asks;
    UINT lastFlags;
};
static void *m_scVtbl[VT_PRESENT1 + 1];

static ULONG STDMETHODCALLTYPE fsc_addref(FakeSc *self) { return ++self->refs; }
static ULONG STDMETHODCALLTYPE fsc_release(FakeSc *self) { return --self->refs; }

static HRESULT STDMETHODCALLTYPE fsc_qi(FakeSc *self, REFIID iid, void **out)
{
    FakeSc *obj = self;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown)) {
        if (self->identity) obj = self->identity;
    } else if (IsEqualIID(iid, &IID_IDXGISwapChain3)) {
        self->sc3Asks++; // the squeeze asks for it first: there's no GPU behind this one
        return E_NOINTERFACE;
    } else if (IsEqualIID(iid, &IID_IDXGISwapChain)) {
        if (self->refuse & 1) return E_NOINTERFACE;
    } else if (IsEqualIID(iid, &IID_IDXGISwapChain1)) {
        if (self->refuse & 2) return E_NOINTERFACE;
    } else {
        return E_NOINTERFACE;
    }
    obj->refs++;
    *out = obj;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fsc_present(FakeSc *self, UINT interval, UINT flags)
{
    self->presents++;
    self->lastFlags = flags;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fsc_present1(FakeSc *self, UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS *p)
{
    self->present1s++;
    self->lastFlags = flags;
    return DXGI_STATUS_OCCLUDED; // passed back as it is
}

static void fake_sc(FakeSc *sc)
{
    for (int i = 0; i <= VT_PRESENT1; i++) m_scVtbl[i] = (void *)fsc_addref; // every slot points into this exe
    m_scVtbl[0] = (void *)fsc_qi;
    m_scVtbl[2] = (void *)fsc_release;
    m_scVtbl[VT_PRESENT] = (void *)fsc_present;
    m_scVtbl[VT_PRESENT1] = (void *)fsc_present1;
    memset(sc, 0, sizeof(*sc));
    sc->lpVtbl = m_scVtbl;
    sc->refs = 1;
}

// GpuApi::Present and RED4ext's Attach.
static int m_gpuPresents, m_lastDeviceIndex;
static void *gpu_present_orig(int32_t *deviceIndex, uint8_t sync, UINT interval)
{
    m_gpuPresents++;
    m_lastDeviceIndex = *deviceIndex;
    return (void *)(uintptr_t)(sync + interval);
}

static uint8_t m_presentCode[16], m_deviceDataVar[8];
static int m_attaches, m_attachFails;
static void *m_attachTarget, *m_attachDetour;

static bool fake_attach(void *handle, void *target, void *detour, void **original)
{
    m_attaches++;
    m_attachTarget = target;
    m_attachDetour = detour;
    if (m_attachFails) return false;
    *original = (void *)gpu_present_orig;
    return true;
}

static bool fake_detach(void *handle, void *target) { return true; }
static R4eHooking m_hooking = { fake_attach, fake_detach };
static R4eSdk m_sdk = { NULL, NULL, &m_hooking, NULL, NULL };

static uintptr_t fake_resolve(uint32_t hash)
{
    return hash == HASH_GPUAPI_PRESENT ? (uintptr_t)m_presentCode : hash == HASH_DEVICE_DATA ? (uintptr_t)m_deviceDataVar : 0;
}
static uintptr_t resolve_present_only(uint32_t hash) { return hash == HASH_GPUAPI_PRESENT ? (uintptr_t)m_presentCode : 0; }

static void hooks_begin(void)
{
    mt_begin();
    m_gpuPresents = m_lastDeviceIndex = m_attaches = m_attachFails = 0;
    m_attachTarget = m_attachDetour = NULL;
}

// ===================================================================================== 3D Mode detection

TEST(stereo_frame_means_3d_on_and_menus_are_squeezed)
{
    mt_begin();
    stereo_frame();
    CHECK_EQ_INT(g.stereo, STEREO_ON);
    CHECK_EQ_INT(want_squeeze(), 0); // gameplay in stereo: untouched
    menu_frame();
    CHECK_EQ_INT(want_squeeze(), 1); // inventory: squeezed
    map_frame();
    CHECK_EQ_INT(want_squeeze(), 1); // the map asks IsAvailable twice but no eye positions
    CHECK_EQ_INT(g.stereo, STEREO_ON);
}

TEST(nothing_is_squeezed_before_3d_mode_is_known)
{
    mt_begin();
    frames(menu_frame, 100); // intro and splash screens with no saved setting
    CHECK_EQ_INT(g.stereo, STEREO_UNKNOWN);
    CHECK_EQ_INT(want_squeeze(), 0);
}

TEST(nothing_is_squeezed_before_the_stand_in_counters_are_found)
{
    mt_begin();
    g.stereo = STEREO_ON;
    g.flatFrame = 1;
    g.deviceCalls = NULL;
    CHECK_EQ_INT(want_squeeze(), 0);
}

TEST(a_run_of_2d_gameplay_frames_means_3d_off)
{
    mt_begin();
    stereo_frame();
    frames(flat_2d_frame, STEREO_OFF_FRAMES - 1);
    CHECK_EQ_INT(g.stereo, STEREO_ON); // a few such frames turn up in Settings too
    flat_2d_frame();
    CHECK_EQ_INT(g.stereo, STEREO_OFF);
    CHECK_EQ_INT(want_squeeze(), 0); // 2D gameplay is never squeezed (the run-1 bug)
    menu_frame();
    CHECK_EQ_INT(want_squeeze(), 0); // nor a menu in 2D
}

TEST(an_interrupted_run_of_2d_frames_does_not_switch_3d_off)
{
    mt_begin();
    stereo_frame();
    for (int i = 0; i < 10; i++) {
        frames(flat_2d_frame, STEREO_OFF_FRAMES / 2);
        menu_frame();
    }
    CHECK_EQ_INT(g.stereo, STEREO_ON);
}

TEST(stereo_frames_switch_3d_back_on)
{
    mt_begin();
    g.stereo = STEREO_OFF;
    stereo_frame();
    CHECK_EQ_INT(g.stereo, STEREO_ON);
}

TEST(the_3d_mode_handler_switches_the_settings_menu_at_once)
{
    mt_begin();
    stereo_frame();
    menu_frame(); // Settings
    CHECK_EQ_INT(want_squeeze(), 1);
    menus_note_3d_mode(0); // switched off there
    CHECK_EQ_INT(g.stereo, STEREO_OFF);
    CHECK_EQ_INT(want_squeeze(), 0);
    menu_frame();
    CHECK_EQ_INT(want_squeeze(), 0);
    menus_note_3d_mode(1); // and on again
    CHECK_EQ_INT(want_squeeze(), 1);
    menus_note_3d_mode(1); // it runs two or three times per switch
    CHECK_EQ_INT(g.stereo, STEREO_ON);
}

TEST(the_3d_mode_handler_sets_3d_mode_before_any_frame_tells)
{
    mt_begin();
    menus_note_3d_mode(1); // start-up, with 3D Mode saved as on
    CHECK_EQ_INT(g.stereo, STEREO_ON);
    menu_frame();
    CHECK_EQ_INT(want_squeeze(), 1);
}

TEST(objects_that_arent_streamline_proxies_are_used_as_they_are)
{
    mt_begin();
    // No sl.interposer.dll in this process: a COM object is returned unchanged.
    IUnknown *obj = NULL;
    REQUIRE(SUCCEEDED(CoCreateFreeThreadedMarshaler(NULL, &obj)));
    CHECK(native_interface(obj, "test object") == (void *)obj);
    IUnknown_Release(obj);
}

TEST(squeeze_on_mode_squeezes_every_frame)
{
    mt_begin();
    g.mode = MODE_ON;
    stereo_frame();
    CHECK_EQ_INT(want_squeeze(), 1);
    g.deviceCalls = NULL;
    CHECK_EQ_INT(want_squeeze(), 1);
}

TEST(the_3d_mode_handler_starts_the_2d_gameplay_run_over)
{
    mt_begin();
    stereo_frame();
    frames(flat_2d_frame, STEREO_OFF_FRAMES - 1);
    menus_note_3d_mode(1);
    frames(flat_2d_frame, STEREO_OFF_FRAMES - 1);
    CHECK_EQ_INT(g.stereo, STEREO_ON);
    flat_2d_frame();
    CHECK_EQ_INT(g.stereo, STEREO_OFF);
}

TEST(a_menu_is_open_on_a_flat_frame_in_3d_mode_once_the_counters_are_found)
{
    mt_begin();
    g.flatFrame = 1;
    g.stereo = STEREO_ON;
    CHECK(menu_open());
    g.stereo = STEREO_OFF;
    CHECK(!menu_open());
    g.stereo = STEREO_UNKNOWN;
    CHECK(!menu_open());
    g.stereo = STEREO_ON;
    g.flatFrame = 0;
    CHECK(!menu_open());
    g.flatFrame = 1;
    g.deviceCalls = NULL;
    CHECK(!menu_open());
}

TEST(set_stereo_logs_only_changes)
{
    mt_begin();
    log_to_file();
    g.gameFrames = 9;
    set_stereo(STEREO_ON, "first");
    set_stereo(STEREO_ON, "again");
    CHECK(log_has("3D Mode now on (first, game frame 9)"));
    CHECK(!log_has("again"));
    set_stereo(STEREO_OFF, "second");
    CHECK(log_has("3D Mode now off (second"));
    set_stereo(STEREO_UNKNOWN, "third");
    CHECK(log_has("3D Mode now unknown (third"));
    CHECK_EQ_INT(g.stereo, STEREO_UNKNOWN);
}

static LONG64 m_eye, m_avail;
static LONG64 counters(int slot) { return slot == SLOT_EYE_POSITION ? m_eye : slot == SLOT_IS_AVAILABLE ? m_avail : 0; }

TEST(each_game_frame_counts_the_calls_since_the_last_one)
{
    mt_begin();
    log_to_file();
    g.level = 2;
    g.deviceCalls = counters;
    m_eye = g.lastEye = 100;
    m_avail = g.lastAvailable = 50;
    m_eye += 2;
    m_avail += 2;
    g.gameFrames = 7;
    note_game_frame();
    CHECK_EQ_INT(g.stereo, STEREO_ON);
    CHECK_EQ_INT(g.flatFrame, 0);
    CHECK_EQ_INT((int)g.lastEye, 102);
    CHECK_EQ_INT((int)g.lastAvailable, 52);
    CHECK(log_has("game frame 7: 2 eye positions, 2 IsAvailable -> stereo"));
    g.gameFrames = 8;
    note_game_frame(); // nothing asked since: a menu
    CHECK_EQ_INT(g.flatFrame, 1);
    CHECK(log_has("game frame 8: 0 eye positions, 0 IsAvailable -> flat"));
}

TEST(stand_in_counters_arent_found_without_the_stand_in)
{
    if (GetModuleHandleW(L"Odyssey3DSDK.dll")) SKIP("the stand-in is already loaded in this process");
    mt_begin();
    g.deviceCalls = NULL;
    g.gameFrames = 1;
    CHECK_EQ_INT(find_device_calls(), 0);
    CHECK(g.deviceCalls == NULL);
}

TEST(stand_in_counters_are_looked_up_every_60_frames_until_found)
{
    mt_begin();
    log_to_file();
    g.deviceCalls = NULL;
    tu_require_dll();
    HMODULE sdk = LoadLibraryExW(tu_dll_path(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    REQUIRE_MSG(sdk != NULL, "LoadLibraryEx(%ls) failed (%lu)", tu_dll_path(), GetLastError());
    g.gameFrames = 2;
    CHECK_EQ_INT(find_device_calls(), 0);
    CHECK(g.deviceCalls == NULL); // not looked up on this frame
    g.gameFrames = 61;
    CHECK_EQ_INT(find_device_calls(), 0); // found, counted from the next frame
    CHECK(g.deviceCalls == (DeviceCallsFn)(void *)GetProcAddress(sdk, "braindance3d_device_calls"));
    CHECK(log_has("the stand-in's call counters found (game frame 61: 0 eye positions, 0 IsAvailable so far)"));
    g.gameFrames = 62;
    CHECK_EQ_INT(find_device_calls(), 1);
    FreeLibrary(sdk);
    CHECK_MSG(GetModuleHandleW(L"Odyssey3DSDK.dll") != NULL, "the stand-in must stay loaded (pinned)");
}

TEST(saved_stereo_is_the_options_own_true_or_false)
{
    struct { const char *json; LONG want; } cases[] = {
        { "{\"name\": \"Odyssey3D_StereoEnabled\", \"value\": true}", STEREO_ON },
        { "{\"name\":\"Odyssey3D_StereoEnabled\",\"value\":false}", STEREO_OFF },
        { "{\"name\": \"Odyssey3D_StereoEnabled\",\r\n\"value\"\t:\r\n true}", STEREO_ON },
        { "{\"name\": \"Odyssey3D_StereoEnabled\"}, {\"value\": true}", STEREO_UNKNOWN },
        { "{\"name\": \"Odyssey3D_StereoEnabled\", \"value\": \"true\"}", STEREO_UNKNOWN },
        { "{\"name\": \"Odyssey3D_StereoEnabled\", \"value\": true", STEREO_UNKNOWN },
        { "{\"name\": \"Odyssey3D_Strength\", \"value\": true}", STEREO_UNKNOWN },
        { "", STEREO_UNKNOWN },
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++)
        CHECK_MSG(parse_saved_stereo(cases[k].json) == cases[k].want, "case %d: %ld, want %ld", k,
                  (long)parse_saved_stereo(cases[k].json), (long)cases[k].want);
}

TEST(read_text_file_reads_at_most_the_buffer_and_ends_it)
{
    wchar_t *dir = tu_tempdir(L"text");
    wchar_t file[TU_PATH], missing[TU_PATH];
    tu_join(file, TU_PATH, dir, L"a.json");
    tu_join(missing, TU_PATH, dir, L"none.json");
    REQUIRE(tu_write_text(file, "0123456789"));
    char small[5], big[32];
    memset(small, 'x', sizeof(small));
    CHECK_EQ_INT(read_text_file(file, small, sizeof(small)), 1);
    CHECK_EQ_STR(small, "0123");
    CHECK_EQ_INT(read_text_file(file, big, sizeof(big)), 1);
    CHECK_EQ_STR(big, "0123456789");
    CHECK_EQ_INT(read_text_file(missing, big, sizeof(big)), 0);
}

// ===================================================================================== UserSettings.json seed

static void write_user_settings(const char *json)
{
    wchar_t *local = tu_tempdir(L"local");
    tu_setenv(L"LOCALAPPDATA", local);
    if (!json) return;
    wchar_t dir[TU_PATH], file[TU_PATH];
    tu_join(dir, TU_PATH, local, L"CD Projekt Red\\Cyberpunk 2077");
    REQUIRE(tu_mkdirs(dir));
    tu_join(file, TU_PATH, dir, L"UserSettings.json");
    REQUIRE(tu_write_text(file, json));
}

// The format of the real file (indentation trimmed).
#define OPT_BEFORE "{\"data\": [{\"group_name\": \"/video/display\", \"options\": [\n" \
                   "{\"name\": \"Odyssey3D_Strength\", \"type\": \"float\", \"value\": 0.7, \"default_value\": 1.0},\n"
#define OPT_AFTER ",\n{\"name\": \"Odyssey3D_Preset\", \"type\": \"name_list\", \"value\": \"Custom\"}]}]}\n"

TEST(saved_setting_true_starts_with_3d_on)
{
    mt_begin();
    write_user_settings(OPT_BEFORE "{\r\n    \"name\": \"Odyssey3D_StereoEnabled\",\r\n    \"type\": \"bool\",\r\n"
                        "    \"value\": true,\r\n    \"default_value\": false\r\n}" OPT_AFTER);
    read_saved_stereo_setting();
    CHECK_EQ_INT(g.stereo, STEREO_ON);
}

TEST(saved_setting_false_starts_with_3d_off)
{
    mt_begin();
    write_user_settings(OPT_BEFORE "{\"name\": \"Odyssey3D_StereoEnabled\", \"type\": \"bool\", \"value\":false}" OPT_AFTER);
    read_saved_stereo_setting();
    CHECK_EQ_INT(g.stereo, STEREO_OFF);
}

TEST(saved_setting_that_is_missing_or_odd_starts_as_unknown)
{
    const char *cases[] = {
        NULL,                                                         // no file
        "",                                                           // empty file
        OPT_BEFORE "{\"name\": \"Odyssey3D_Preset\"}" OPT_AFTER,       // no such option
        "{\"name\": \"Odyssey3D_StereoEnabled\", \"type\": \"bool\"}, {\"value\": true}", // value of another option
        "{\"name\": \"Odyssey3D_StereoEnabled\", \"value\": 1}",       // not a bool
        "{\"name\": \"Odyssey3D_StereoEnabled\", \"value\": tr",       // truncated
        "{\"name\": \"Odyssey3D_StereoEnabled\", \"val",               // truncated before the value
        "\"Odyssey3D_StereoEnabled\"",                                 // just the name
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
        mt_begin();
        write_user_settings(cases[k]);
        read_saved_stereo_setting();
        CHECK_MSG(g.stereo == STEREO_UNKNOWN, "case %d: stereo %ld, want unknown", k, (long)g.stereo);
    }
}

TEST(no_localappdata_starts_as_unknown)
{
    mt_begin();
    tu_setenv(L"LOCALAPPDATA", NULL);
    read_saved_stereo_setting();
    CHECK_EQ_INT(g.stereo, STEREO_UNKNOWN);
}

// ===================================================================================== settings file

static void read_settings_text(const char *ini)
{
    wchar_t *dir = tu_tempdir(L"ini");
    wchar_t file[TU_PATH];
    tu_join(file, TU_PATH, dir, L"Braindance3D-Settings.ini");
    if (ini) REQUIRE(tu_write_text(file, ini));
    read_menu_settings(dir);
}

TEST(menu_settings_squeeze_values)
{
    struct { const char *ini; int mode; } cases[] = {
        { NULL, MODE_AUTO },
        { "[Menus]\nSqueeze = auto\n", MODE_AUTO },
        { "[Menus]\nSqueeze = off\n", MODE_OFF },
        { "[Menus]\r\nSqueeze=OFF ; no hooks\r\n", MODE_OFF },
        { "[Menus]\nSqueeze = on\n", MODE_ON },
        { "[Menus]\nSqueeze = on;test\n", MODE_ON },
        { "[Menus]\nSqueeze = offset\n", MODE_AUTO },
        { "[Menus]\nSqueeze = once\n", MODE_AUTO },
        { "[Menus]\nSqueeze = garbage\n", MODE_AUTO },
        { "[Display]\nSqueeze = off\n", MODE_AUTO }, // wrong section
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
        read_settings_text(cases[k].ini);
        CHECK_MSG(g.mode == cases[k].mode, "settings \"%s\" -> mode %d, want %d", cases[k].ini ? cases[k].ini : "(none)",
                  g.mode, cases[k].mode);
    }
}

TEST(menu_settings_log_level_matches_the_stand_in)
{
    struct { const char *ini; int level; } cases[] = {
        { NULL, 1 }, { "[Log]\nLevel = 0\n", 0 }, { "[Log]\nLevel = 1\n", 1 }, { "[Log]\nLevel = 2\n", 2 },
        { "[Log]\nLevel = 3\n", 2 }, { "[Log]\nLevel = -1\n", 1 }, { "[Log]\nLevel = x\n", 1 },
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
        read_settings_text(cases[k].ini);
        CHECK_MSG(g.level == cases[k].level, "settings \"%s\" -> level %d, want %d", cases[k].ini ? cases[k].ini : "(none)",
                  g.level, cases[k].level);
    }
}

// ===================================================================================== logging

TEST(plugin_log_writes_only_up_to_the_log_level)
{
    mt_begin();
    log_to_file();
    g.level = 1;
    plugin_log(1, "normal line %d", 1);
    plugin_log(2, "verbose line %d", 2);
    CHECK(log_has("] normal line 1\r\n")); // a text-mode file: Windows line ends
    CHECK(!log_has("verbose line"));
    g.level = 2;
    plugin_log(2, "verbose line %d", 3);
    CHECK(log_has("verbose line 3"));
    g.level = 0;
    plugin_log(1, "with logging off");
    CHECK(!log_has("with logging off"));
    const char *text = log_text();
    CHECK_MSG(text[4] == '-' && text[7] == '-' && text[10] == ' ' && text[23] == ' ' && text[24] == '[',
              "a line starts with the date, time and thread: %.40s", text);
}

TEST(logging_without_a_log_file_does_nothing)
{
    mt_begin();
    g.level = 2;
    plugin_log(1, "no file");
    VLOG("no file either");
    CHECK(g.log == NULL);
}

TEST(fail_turns_the_squeeze_off_and_logs_the_first_reason_only)
{
    mt_begin();
    log_to_file();
    CHECK_EQ_INT(fail("first thing", E_FAIL), 0);
    CHECK_EQ_INT(g.failed, 1);
    CHECK_EQ_INT(fail("second thing", E_POINTER), 0);
    CHECK(log_has("ERROR: first thing (hr=0x80004005); menu squeeze is off for the rest of this session"));
    CHECK(!log_has("second thing"));
}

TEST(hooks_log_keeps_the_previous_session_as_the_old_log)
{
    mt_begin();
    wchar_t *dir = tu_tempdir(L"rotate");
    wchar_t log[TU_PATH], old[TU_PATH];
    tu_join(log, TU_PATH, dir, L"Braindance3D-hooks.log");
    tu_join(old, TU_PATH, dir, L"Braindance3D-hooks.old.log");
    REQUIRE(tu_write_text(log, "last session\n"));
    REQUIRE(tu_write_text(old, "the one before\n"));
    FILE *f = open_hooks_log_in(dir);
    REQUIRE(f != NULL);
    fclose(f);
    CHECK_EQ_INT((int)tu_file_size(log), 0);
    char *text = tu_read_file(old, NULL);
    CHECK_EQ_STR(text, "last session\n");
    free(text);
}

TEST(hooks_log_goes_to_local_app_data_when_the_game_folder_cant_take_it)
{
    mt_begin();
    wchar_t *local = tu_tempdir(L"local");
    tu_setenv(L"LOCALAPPDATA", local);
    wchar_t missing[TU_PATH], want[TU_PATH];
    tu_join(missing, TU_PATH, tu_tempdir(L"game"), L"no such folder");
    open_hooks_log(missing);
    REQUIRE(g.log != NULL);
    tt_defer(close_log, NULL);
    tu_join(want, TU_PATH, local, L"Braindance3D\\Braindance3D-hooks.log");
    CHECK(tu_exists(want));
}

TEST(local_app_data_dir_is_made_under_localappdata)
{
    wchar_t *local = tu_tempdir(L"local");
    tu_setenv(L"LOCALAPPDATA", local);
    wchar_t out[MAX_PATH], want[TU_PATH];
    REQUIRE_EQ_INT(local_app_data_dir(out), 1);
    tu_join(want, TU_PATH, local, L"Braindance3D");
    CHECK_EQ_WSTR(out, want);
    CHECK(tu_is_dir(out));
    tu_setenv(L"LOCALAPPDATA", NULL);
    CHECK_EQ_INT(local_app_data_dir(out), 0);
}

// ===================================================================================== settings, one value at a time

TEST(value_is_matches_the_whole_word_in_any_case)
{
    CHECK(value_is(L"off", L"off"));
    CHECK(value_is(L"OFF", L"off"));
    CHECK(value_is(L"off ; no hooks", L"off"));
    CHECK(value_is(L"off\t# no hooks", L"off"));
    CHECK(value_is(L"on;test", L"on"));
    CHECK(!value_is(L"offset", L"off"));
    CHECK(!value_is(L"of", L"off"));
    CHECK(!value_is(L"once", L"on"));
    CHECK(!value_is(L"", L"on"));
}

TEST(log_level_values)
{
    struct { const wchar_t *v; int want; } cases[] = {
        { L"0", 0 }, { L"1", 1 }, { L"2", 2 }, { L"7", 2 }, { L" 2", 2 }, { L"\t0", 0 },
        { L"0 ; off", 0 }, { L"-1", 1 }, { L"", 1 }, { L"x", 1 },
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++)
        CHECK_MSG(parse_log_level(cases[k].v) == cases[k].want, "\"%ls\" -> %d, want %d", cases[k].v,
                  parse_log_level(cases[k].v), cases[k].want);
}

TEST(squeeze_mode_values)
{
    struct { const wchar_t *v; int want; } cases[] = {
        { L"off", MODE_OFF }, { L"Off ;x", MODE_OFF }, { L"on", MODE_ON }, { L"ON", MODE_ON },
        { L"auto", MODE_AUTO }, { L"", MODE_AUTO }, { L"offset", MODE_AUTO }, { L"1", MODE_AUTO },
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++)
        CHECK_MSG(parse_squeeze_mode(cases[k].v) == cases[k].want, "\"%ls\" -> %d, want %d", cases[k].v,
                  parse_squeeze_mode(cases[k].v), cases[k].want);
}

TEST(mode_names_for_the_log)
{
    CHECK_EQ_STR(mode_name(MODE_OFF), "off");
    CHECK_EQ_STR(mode_name(MODE_AUTO), "auto");
    CHECK_EQ_STR(mode_name(MODE_ON), "on (every frame)");
    CHECK_EQ_STR(stereo_name(STEREO_ON), "on");
    CHECK_EQ_STR(stereo_name(STEREO_OFF), "off");
    CHECK_EQ_STR(stereo_name(STEREO_UNKNOWN), "unknown");
}

// ===================================================================================== memory checks

TEST(readable_needs_committed_accessible_memory_for_the_whole_range)
{
    int local = 0;
    CHECK(readable(&local, sizeof(local)));
    CHECK(!readable(NULL, 1));
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t page = si.dwPageSize;
    uint8_t *p = VirtualAlloc(NULL, 2 * page, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(p != NULL);
    REQUIRE(VirtualAlloc(p, page, MEM_COMMIT, PAGE_READWRITE) != NULL);
    CHECK(readable(p, page));
    CHECK(!readable(p + page - 4, 8)); // runs into the page that is only reserved
    CHECK(!readable(p + page, 1));
    DWORD old;
    REQUIRE(VirtualProtect(p, page, PAGE_NOACCESS, &old));
    CHECK(!readable(p, 1));
    REQUIRE(VirtualProtect(p, page, PAGE_READONLY | PAGE_GUARD, &old));
    CHECK(!readable(p, 1));
    VirtualFree(p, 0, MEM_RELEASE);
}

TEST(module_at_and_module_of_name_the_module_that_holds_an_address)
{
    char name[MAX_PATH];
    void *rtl = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    REQUIRE(rtl != NULL);
    CHECK_EQ_PTR(module_at((void *)fsc_qi), GetModuleHandleW(NULL));
    CHECK_EQ_PTR(module_at(rtl), GetModuleHandleW(L"ntdll.dll"));
    CHECK_MSG(_stricmp(module_of(rtl, name, sizeof(name)), "ntdll.dll") == 0, "module_of: %s", name);
    CHECK_MSG(_stricmp(module_of((void *)fsc_qi, name, sizeof(name)), "test_menus.exe") == 0, "module_of: %s", name);
    void *heap = malloc(16);
    CHECK(module_at(heap) == NULL);
    CHECK_EQ_STR(module_of(heap, name, sizeof(name)), "?");
    free(heap);
}

TEST(looks_like_com_wants_a_vtable_that_points_into_a_module)
{
    FakeSc sc;
    fake_sc(&sc);
    CHECK(looks_like_com(&sc));
    CHECK(!looks_like_com(NULL));
    void *heapVtbl[3] = { malloc(1), malloc(1), malloc(1) };
    struct { void **vt; } outside = { heapVtbl }, unreadable = { (void **)(uintptr_t)8 };
    CHECK(!looks_like_com(&outside));
    CHECK(!looks_like_com(&unreadable));
    for (int i = 0; i < 3; i++) free(heapVtbl[i]);
}

// ===================================================================================== the game's device data

TEST(device_data_is_read_through_the_games_variable)
{
    mt_begin();
    CHECK(device_data() == 0); // not resolved
    fake_device_data();
    CHECK(device_data() == (uintptr_t)m_dd);
    m_ddVar = 0; // resolved, not set up yet
    CHECK(device_data() == 0);
    CHECK(swap_chain_entry(0) == NULL);
    CHECK(direct_queue_entry() == NULL);
}

TEST(swap_chain_entries_are_read_at_the_games_offsets)
{
    mt_begin();
    fake_device_data();
    int a, b;
    set_swap_chain_entry(0, &a);
    set_swap_chain_entry(DD_SWAPCHAIN_COUNT - 1, &b);
    CHECK_EQ_PTR(swap_chain_entry(0), &a);
    CHECK_EQ_PTR(swap_chain_entry(DD_SWAPCHAIN_COUNT - 1), &b);
    CHECK(swap_chain_entry(1) == NULL);
    CHECK(swap_chain_entry(-1) == NULL);
    CHECK(swap_chain_entry(DD_SWAPCHAIN_COUNT) == NULL);
}

TEST(direct_queue_entry_is_read_at_the_games_offset)
{
    mt_begin();
    fake_device_data();
    int q;
    set_direct_queue(&q);
    CHECK_EQ_PTR(direct_queue_entry(), &q);
}

// ===================================================================================== the game's direct queue

// A command queue: answers ID3D12CommandQueue (unless refused) and reports its type.
typedef struct {
    void **lpVtbl;
    LONG refs;
    D3D12_COMMAND_LIST_TYPE type;
    int refuse;
} FakeQueue;
static void *m_qVtbl[sizeof(ID3D12CommandQueueVtbl) / sizeof(void *)];

static ULONG STDMETHODCALLTYPE fq_addref(FakeQueue *self) { return ++self->refs; }
static ULONG STDMETHODCALLTYPE fq_release(FakeQueue *self) { return --self->refs; }

static HRESULT STDMETHODCALLTYPE fq_qi(FakeQueue *self, REFIID iid, void **out)
{
    *out = NULL;
    if (!IsEqualIID(iid, &IID_IUnknown) && (!IsEqualIID(iid, &IID_ID3D12CommandQueue) || self->refuse)) return E_NOINTERFACE;
    self->refs++;
    *out = self;
    return S_OK;
}

// ID3D12CommandQueue::GetDesc the C++ way: this in RCX, the return buffer in RDX.
static D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE fq_getdesc(FakeQueue *self, D3D12_COMMAND_QUEUE_DESC *ret)
{
    memset(ret, 0, sizeof(*ret));
    ret->Type = self->type;
    return ret;
}

static void fake_queue(FakeQueue *q, D3D12_COMMAND_LIST_TYPE type)
{
    for (size_t i = 0; i < sizeof(m_qVtbl) / sizeof(void *); i++) m_qVtbl[i] = (void *)fq_addref;
    m_qVtbl[0] = (void *)fq_qi;
    m_qVtbl[2] = (void *)fq_release;
    m_qVtbl[offsetof(ID3D12CommandQueueVtbl, GetDesc) / sizeof(void *)] = (void *)fq_getdesc;
    memset(q, 0, sizeof(*q));
    q->lpVtbl = m_qVtbl;
    q->refs = 1;
    q->type = type;
}

TEST(game_direct_queue_is_the_queue_in_the_device_data)
{
    mt_begin();
    log_to_file();
    fake_device_data();
    FakeQueue q;
    fake_queue(&q, D3D12_COMMAND_LIST_TYPE_DIRECT);
    set_direct_queue(&q);
    CHECK_EQ_PTR(game_direct_queue(), &q);
    CHECK_EQ_INT(q.refs, 2); // ours
    CHECK_EQ_INT(g.failed, 0);
    CHECK(log_has("(vtable in test_menus.exe)"));
}

TEST(game_direct_queue_turns_the_squeeze_off_for_anything_else)
{
    struct { int what; const char *why; } cases[] = {
        { 0, "the game's direct command queue doesn't look valid" }, // no queue yet
        { 1, "the game's direct command queue doesn't look valid" }, // not a COM object
        { 2, "QueryInterface(ID3D12CommandQueue) on the game's direct queue" },
        { 3, "the game's queue at the direct-queue offset isn't a direct queue" },
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
        mt_begin();
        log_to_file();
        fake_device_data();
        FakeQueue q;
        int notCom = 0;
        fake_queue(&q, cases[k].what == 3 ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT);
        q.refuse = cases[k].what == 2;
        set_direct_queue(cases[k].what == 0 ? NULL : cases[k].what == 1 ? (void *)&notCom : (void *)&q);
        CHECK_MSG(game_direct_queue() == NULL, "case %d gave a queue", k);
        CHECK_MSG(g.failed == 1, "case %d didn't turn the squeeze off", k);
        CHECK_MSG(log_has(cases[k].why), "case %d: no \"%s\" in the log", k, cases[k].why);
        CHECK_MSG(q.refs == 1, "case %d kept %ld references", k, (long)q.refs - 1);
    }
}

TEST(init_d3d_runs_once_and_needs_the_games_queue)
{
    mt_begin();
    log_to_file();
    fake_device_data();
    CHECK_EQ_INT(init_d3d(), 0);
    CHECK_EQ_INT(g.failed, 1);
    g.failed = 0;
    g.device = (ID3D12Device *)&m_ddVar; // already set up: not looked at again
    CHECK_EQ_INT(init_d3d(), 1);
    g.device = NULL;
}

TEST(barrier_is_a_transition_of_every_subresource)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0xAB, sizeof(b));
    ID3D12Resource *r = (ID3D12Resource *)&b;
    barrier(&b, r, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CHECK_EQ_INT(b.Type, D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
    CHECK_EQ_INT(b.Flags, D3D12_RESOURCE_BARRIER_FLAG_NONE);
    CHECK_EQ_PTR(b.Transition.pResource, r);
    CHECK_EQ_INT(b.Transition.Subresource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    CHECK_EQ_INT(b.Transition.StateBefore, D3D12_RESOURCE_STATE_PRESENT);
    CHECK_EQ_INT(b.Transition.StateAfter, D3D12_RESOURCE_STATE_COPY_SOURCE);
}

TEST(a_swap_chain_without_back_buffer_access_turns_the_squeeze_off)
{
    mt_begin();
    log_to_file();
    FakeSc sc;
    fake_sc(&sc);
    UINT index = 99;
    CHECK(current_back_buffer((IDXGISwapChain *)&sc, &index) == NULL);
    CHECK_EQ_INT(g.failed, 1);
    CHECK(log_has("QueryInterface(IDXGISwapChain3)"));
    CHECK_EQ_INT(sc.refs, 1);
}

// ===================================================================================== the game's swap chain

TEST(same_com_object_compares_identities)
{
    FakeSc a, b, wrapper;
    fake_sc(&a);
    fake_sc(&b);
    fake_sc(&wrapper);
    wrapper.identity = &a;
    int notCom = 0;
    CHECK(same_com_object(&a, &a));
    CHECK(!same_com_object(&a, &b));
    CHECK(same_com_object(&wrapper, &a));
    CHECK(!same_com_object(&a, &notCom));
    CHECK(!same_com_object(&a, NULL));
    CHECK_EQ_INT(a.refs, 1); // every reference given back
    CHECK_EQ_INT(b.refs, 1);
    CHECK_EQ_INT(wrapper.refs, 1);
}

TEST(only_the_swap_chain_in_the_games_entry_is_the_games)
{
    mt_begin();
    fake_device_data();
    FakeSc game, other;
    fake_sc(&game);
    fake_sc(&other);
    g.scIndex = 2;
    CHECK(!is_game_swap_chain((IDXGISwapChain *)&game)); // the entry is empty
    set_swap_chain_entry(2, &game);
    CHECK(is_game_swap_chain((IDXGISwapChain *)&game));
    CHECK(!is_game_swap_chain((IDXGISwapChain *)&other));
    g.scIndex = 3;
    CHECK(!is_game_swap_chain((IDXGISwapChain *)&game));
}

TEST(patch_slot_swaps_the_entry_and_keeps_the_page_protection)
{
    DWORD protections[] = { PAGE_READONLY, PAGE_READWRITE, PAGE_EXECUTE_READ };
    for (int k = 0; k < 3; k++) {
        void **page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        REQUIRE(page != NULL);
        page[5] = (void *)fsc_present;
        DWORD old;
        REQUIRE(VirtualProtect(page, 4096, protections[k], &old));
        void *orig = NULL;
        CHECK(patch_slot(&page[5], (void *)hook_present, &orig));
        CHECK_EQ_PTR(orig, (void *)fsc_present);
        CHECK_EQ_PTR(page[5], (void *)hook_present);
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(&page[5], &mbi, sizeof(mbi));
        CHECK_MSG(mbi.Protect == protections[k], "protection 0x%lx after the patch, want 0x%lx", mbi.Protect, protections[k]);
        VirtualFree(page, 0, MEM_RELEASE);
    }
}

TEST(game_swap_chain_is_the_entry_for_the_device_index)
{
    mt_begin();
    log_to_file();
    fake_device_data();
    FakeSc sc;
    fake_sc(&sc);
    set_swap_chain_entry(4, &sc);
    int32_t idx = 5;
    CHECK_EQ_PTR(game_swap_chain(&idx), &sc);
    CHECK_EQ_INT(sc.refs, 2); // the caller's
    CHECK_EQ_INT(g.scIndex, 4);
    CHECK_EQ_INT(g.failed, 0);
}

TEST(game_swap_chain_turns_the_squeeze_off_when_the_entry_isnt_usable)
{
    struct { int setUp, index, entry; const char *why; } cases[] = {
        { 1, 0, 1, "unexpected device index" },
        { 1, DD_SWAPCHAIN_COUNT + 1, 1, "unexpected device index" },
        { 0, 1, 1, "the game's device data isn't set up" },
        { 1, 1, 0, "the swap chain in the game's device data doesn't look valid" },
        { 1, 1, 2, "QueryInterface(IDXGISwapChain) on the game's swap chain" },
        { 1, -1, 1, "unreadable device index" },
    };
    for (int k = 0; k < (int)(sizeof(cases) / sizeof(cases[0])); k++) {
        mt_begin();
        log_to_file();
        fake_device_data();
        FakeSc sc;
        fake_sc(&sc);
        sc.refuse = cases[k].entry == 2 ? 1 : 0;
        int notCom = 0;
        for (int i = 0; i < DD_SWAPCHAIN_COUNT; i++) set_swap_chain_entry(i, cases[k].entry ? (void *)&sc : (void *)&notCom);
        if (!cases[k].setUp) m_ddVar = 0;
        int32_t idx = cases[k].index;
        CHECK_MSG(game_swap_chain(cases[k].index < 0 ? NULL : &idx) == NULL, "case %d gave a swap chain", k);
        CHECK_MSG(g.failed == 1, "case %d didn't turn the squeeze off", k);
        CHECK_MSG(log_has(cases[k].why), "case %d: no \"%s\" in the log", k, cases[k].why);
        CHECK_MSG(sc.refs == 1, "case %d kept a reference", k);
    }
}

TEST(present_and_present1_are_patched)
{
    mt_begin();
    log_to_file();
    FakeSc sc;
    fake_sc(&sc);
    CHECK_EQ_INT(patch_present_slots((IDXGISwapChain *)&sc), 1);
    CHECK_EQ_PTR(m_scVtbl[VT_PRESENT], (void *)hook_present);
    CHECK_EQ_PTR(m_scVtbl[VT_PRESENT1], (void *)hook_present1);
    CHECK_EQ_PTR((void *)g.presentOrig, (void *)fsc_present);
    CHECK_EQ_PTR((void *)g.present1Orig, (void *)fsc_present1);
    CHECK_EQ_INT(sc.refs, 1);
    CHECK(log_has("Present1 patched too (in test_menus.exe)"));
}

TEST(present1_is_left_alone_without_idxgiswapchain1)
{
    mt_begin();
    log_to_file();
    FakeSc sc;
    fake_sc(&sc);
    sc.refuse = 2;
    CHECK_EQ_INT(patch_present_slots((IDXGISwapChain *)&sc), 1);
    CHECK_EQ_PTR(m_scVtbl[VT_PRESENT], (void *)hook_present);
    CHECK_EQ_PTR(m_scVtbl[VT_PRESENT1], (void *)fsc_present1);
    CHECK(g.present1Orig == NULL);
}

TEST(patch_swap_chain_patches_the_games_swap_chain_without_keeping_it)
{
    mt_begin();
    log_to_file();
    fake_device_data();
    FakeSc sc;
    fake_sc(&sc);
    set_swap_chain_entry(0, &sc);
    int32_t idx = 1;
    patch_swap_chain(&idx);
    CHECK_EQ_PTR(m_scVtbl[VT_PRESENT], (void *)hook_present);
    CHECK_EQ_INT(sc.refs, 1);
    CHECK(log_has("game swap chain "));
    CHECK(log_has("swap chain Present patched; the squeeze runs while a menu is open in 3D Mode"));
}

// ===================================================================================== presenting

// The game's swap chain at device index 1, with its Present slots patched.
static void patched_game_swap_chain(FakeSc *sc)
{
    fake_device_data();
    fake_sc(sc);
    set_swap_chain_entry(0, sc);
    REQUIRE(patch_present_slots((IDXGISwapChain *)sc));
}

TEST(present_hooks_pass_the_call_on_unchanged)
{
    mt_begin();
    log_to_file();
    FakeSc sc;
    patched_game_swap_chain(&sc);
    CHECK_EQ_INT(IDXGISwapChain_Present((IDXGISwapChain *)&sc, 1, DXGI_PRESENT_DO_NOT_WAIT), S_OK);
    CHECK_EQ_INT(sc.presents, 1);
    CHECK_EQ_INT(sc.lastFlags, DXGI_PRESENT_DO_NOT_WAIT);
    CHECK_EQ_INT((int)g.frames, 1);
    DXGI_PRESENT_PARAMETERS pp = { 0 };
    CHECK_EQ_INT(IDXGISwapChain1_Present1((IDXGISwapChain1 *)&sc, 0, 0, &pp), DXGI_STATUS_OCCLUDED);
    CHECK_EQ_INT(sc.present1s, 1);
    CHECK_EQ_INT((int)g.frames, 2);
}

TEST(test_presents_and_other_swap_chains_are_not_counted)
{
    mt_begin();
    log_to_file();
    FakeSc sc;
    patched_game_swap_chain(&sc);
    FakeSc other = sc; // another swap chain on the same (patched) vtable
    other.presents = 0;
    IDXGISwapChain_Present((IDXGISwapChain *)&sc, 0, DXGI_PRESENT_TEST);
    IDXGISwapChain1_Present1((IDXGISwapChain1 *)&sc, 0, DXGI_PRESENT_TEST, NULL);
    IDXGISwapChain_Present((IDXGISwapChain *)&other, 0, 0);
    CHECK_EQ_INT(sc.presents, 1);
    CHECK_EQ_INT(sc.present1s, 1);
    CHECK_EQ_INT(other.presents, 1);
    CHECK_EQ_INT((int)g.frames, 0);
}

TEST(a_present_from_inside_a_squeeze_passes_straight_through)
{
    mt_begin();
    log_to_file();
    FakeSc sc;
    patched_game_swap_chain(&sc);
    g.busy = 1;
    IDXGISwapChain_Present((IDXGISwapChain *)&sc, 0, 0);
    CHECK_EQ_INT(sc.presents, 1);
    CHECK_EQ_INT((int)g.frames, 0);
    CHECK_EQ_INT(g.busy, 1);
}

TEST(squeeze_runs_at_most_once_per_game_frame)
{
    mt_begin();
    log_to_file();
    FakeSc sc;
    patched_game_swap_chain(&sc);
    g.mode = MODE_ON;
    g.device = (ID3D12Device *)&m_ddVar; // "set up": the squeeze goes straight to the back buffer, which this one can't give
    g.gameFrames = 1;
    IDXGISwapChain_Present((IDXGISwapChain *)&sc, 0, 0);
    CHECK_EQ_INT(sc.sc3Asks, 1);
    CHECK(log_has("Present: squeeze ON (frame 1)"));
    g.failed = 0;
    IDXGISwapChain_Present((IDXGISwapChain *)&sc, 0, 0); // the same game frame presented again
    CHECK_EQ_INT(sc.sc3Asks, 1);
    g.gameFrames = 2;
    IDXGISwapChain1_Present1((IDXGISwapChain1 *)&sc, 0, 0, NULL);
    CHECK_EQ_INT(sc.sc3Asks, 2);
    CHECK_EQ_INT(g.busy, 0);
    g.device = NULL;
}

TEST(a_failed_squeeze_leaves_presenting_to_the_game)
{
    mt_begin();
    log_to_file();
    FakeSc sc;
    patched_game_swap_chain(&sc); // and no direct queue in the device data
    g.mode = MODE_ON;
    g.gameFrames = 1;
    CHECK_EQ_INT(IDXGISwapChain_Present((IDXGISwapChain *)&sc, 0, 0), S_OK);
    CHECK_EQ_INT(g.failed, 1);
    g.gameFrames = 2;
    CHECK_EQ_INT(IDXGISwapChain_Present((IDXGISwapChain *)&sc, 0, 0), S_OK);
    CHECK_EQ_INT(sc.presents, 2);
    CHECK(log_has("Present: squeeze off (frame 2)"));
}

TEST(summary_is_logged_once_a_second_at_level_2)
{
    mt_begin();
    log_to_file();
    g.frames = 5;
    g.squeezed = 3;
    g.level = 1;
    g.lastSummary = GetTickCount() - 5000;
    log_summary();
    CHECK(!log_has("summary"));
    g.level = 2;
    log_summary();
    CHECK(log_has("summary: 5 frames presented, 3 squeezed"));
    g.frames = 6;
    log_summary();
    CHECK(!log_has("summary: 6"));
}

// ===================================================================================== GpuApi::Present

TEST(follow_swap_chain_moves_to_a_new_device_index)
{
    mt_begin();
    log_to_file();
    g.patched = 1;
    int32_t idx = 3;
    follow_swap_chain(&idx);
    CHECK_EQ_INT(g.scIndex, 2);
    CHECK(log_has("game swap chain now at device index 3 (was 1)"));
    int32_t odd[] = { 0, DD_SWAPCHAIN_COUNT + 1, -5 };
    for (int k = 0; k < 3; k++) {
        follow_swap_chain(&odd[k]);
        CHECK_EQ_INT(g.scIndex, 2);
    }
    follow_swap_chain(NULL);
    CHECK_EQ_INT(g.scIndex, 2);
}

TEST(gpu_present_hook_patches_once_counts_frames_and_calls_the_original)
{
    hooks_begin();
    log_to_file();
    fake_device_data();
    FakeSc sc;
    fake_sc(&sc);
    set_swap_chain_entry(1, &sc);
    g.gpuPresentOrig = gpu_present_orig;
    int32_t idx = 2;
    CHECK_EQ_PTR(hook_gpu_present(&idx, 1, 2), (void *)3);
    CHECK_EQ_INT(m_gpuPresents, 1);
    CHECK_EQ_INT(m_lastDeviceIndex, 2);
    CHECK_EQ_INT(g.patched, 1);
    CHECK_EQ_PTR(m_scVtbl[VT_PRESENT], (void *)hook_present);
    CHECK_EQ_INT(g.scIndex, 1);
    CHECK_EQ_INT((int)g.gameFrames, 1);
    m_scVtbl[VT_PRESENT] = (void *)fsc_present; // a second patch would show here
    hook_gpu_present(&idx, 0, 0);
    CHECK_EQ_PTR(m_scVtbl[VT_PRESENT], (void *)fsc_present);
    CHECK_EQ_INT((int)g.gameFrames, 2);
    CHECK_EQ_INT(m_gpuPresents, 2);
}

TEST(gpu_present_hook_doesnt_patch_after_a_failure)
{
    hooks_begin();
    log_to_file();
    fake_device_data();
    FakeSc sc;
    fake_sc(&sc);
    set_swap_chain_entry(0, &sc);
    g.gpuPresentOrig = gpu_present_orig;
    g.failed = 1;
    int32_t idx = 1;
    hook_gpu_present(&idx, 0, 0);
    CHECK_EQ_INT(g.patched, 0);
    CHECK_EQ_PTR(m_scVtbl[VT_PRESENT], (void *)fsc_present);
    CHECK_EQ_INT(m_gpuPresents, 1);
}

TEST(gpu_present_hook_reads_the_stand_ins_counters_in_auto_mode_only)
{
    hooks_begin();
    log_to_file();
    g.gpuPresentOrig = gpu_present_orig;
    g.patched = 1;
    g.deviceCalls = counters;
    m_eye = m_avail = 0;
    int32_t idx = 1;
    m_eye += 2;
    m_avail += 2;
    hook_gpu_present(&idx, 0, 0);
    CHECK_EQ_INT(g.stereo, STEREO_ON);
    g.mode = MODE_ON;
    g.stereo = STEREO_OFF;
    m_eye += 2;
    hook_gpu_present(&idx, 0, 0);
    CHECK_EQ_INT(g.stereo, STEREO_OFF); // not read
}

// ===================================================================================== loading

TEST(red4ext_resolver_needs_red4ext)
{
    mt_begin();
    log_to_file();
    CHECK(red4ext_resolver() == NULL); // no RED4ext.dll in this process
    CHECK_EQ_INT(g.failed, 1);
    CHECK(log_has("RED4ext.dll has no RED4ext_ResolveAddress"));
}

TEST(hook_game_present_hooks_gpuapi_present)
{
    hooks_begin();
    log_to_file();
    CHECK_EQ_INT(hook_game_present((void *)0x1234, &m_sdk, fake_resolve), 1);
    CHECK_EQ_PTR(m_attachTarget, m_presentCode);
    CHECK_EQ_PTR(m_attachDetour, (void *)hook_gpu_present);
    CHECK_EQ_PTR((void *)g.gpuPresentOrig, (void *)gpu_present_orig);
    CHECK(g.deviceData == (uintptr_t)m_deviceDataVar);
    CHECK(log_has("GpuApi::Present hooked; waiting for the first frame"));
}

TEST(hook_game_present_needs_both_addresses_and_the_hook)
{
    hooks_begin();
    log_to_file();
    CHECK_EQ_INT(hook_game_present(NULL, &m_sdk, resolve_present_only), 0);
    CHECK_EQ_INT(m_attaches, 0);
    CHECK_EQ_INT(g.failed, 1);
    CHECK(log_has("RED4ext couldn't resolve the game addresses"));
    hooks_begin();
    log_to_file();
    m_attachFails = 1;
    CHECK_EQ_INT(hook_game_present(NULL, &m_sdk, fake_resolve), 0);
    CHECK_EQ_INT(m_attaches, 1);
    CHECK_EQ_INT(g.failed, 1);
    CHECK(log_has("RED4ext couldn't hook GpuApi::Present"));
}

// An install folder with this settings file (none for NULL), and LOCALAPPDATA without a UserSettings.json.
static wchar_t *install_dir_with(const char *ini)
{
    tu_setenv(L"LOCALAPPDATA", tu_tempdir(L"local"));
    wchar_t *dir = tu_tempdir(L"install");
    wchar_t file[TU_PATH];
    tu_join(file, TU_PATH, dir, L"Braindance3D-Settings.ini");
    if (ini) REQUIRE(tu_write_text(file, ini));
    tu_join(m_logPath, TU_PATH, dir, L"Braindance3D-hooks.log");
    tt_defer(close_log, NULL);
    return dir;
}

TEST(menus_load_with_squeeze_off_installs_nothing)
{
    hooks_begin();
    wchar_t *dir = install_dir_with("[Menus]\nSqueeze = off\n");
    CHECK_EQ_INT(menus_load(NULL, &m_sdk, dir), 1);
    CHECK_EQ_INT(m_attaches, 0);
    CHECK(log_has("==== 3D Braindance Edition menu squeeze, mode off, log level 1"));
    CHECK(log_has("[Menus] Squeeze = off: no squeeze hooks installed"));
}

TEST(menus_load_with_log_level_0_writes_no_log)
{
    hooks_begin();
    wchar_t *dir = install_dir_with("[Log]\nLevel = 0\n[Menus]\nSqueeze = off\n");
    CHECK_EQ_INT(menus_load(NULL, &m_sdk, dir), 1);
    CHECK(g.log == NULL);
    CHECK(!tu_exists(m_logPath));
}

TEST(menus_load_without_a_hooking_interface_gives_up)
{
    hooks_begin();
    wchar_t *dir = install_dir_with(NULL);
    R4eSdk noHooks = { 0 };
    CHECK_EQ_INT(menus_load(NULL, &noHooks, dir), 0);
    CHECK_EQ_INT(g.failed, 1);
    CHECK(log_has("RED4ext didn't pass a hooking interface"));
    CHECK(log_has("3D Mode setting: "));
}

TEST(menus_load_without_red4ext_gives_up)
{
    hooks_begin();
    wchar_t *dir = install_dir_with("[Log]\nLevel = 2\n");
    CHECK_EQ_INT(menus_load(NULL, &m_sdk, dir), 0);
    CHECK_EQ_INT(m_attaches, 0);
    CHECK(log_has("mode auto, log level 2"));
    CHECK(log_has("RED4ext.dll has no RED4ext_ResolveAddress"));
}

// ===================================================================================== the squeeze shaders

static void release_deferred(void *p)
{
    if (p) IUnknown_Release((IUnknown *)p);
}

#define RELEASE(p) do { if (p) IUnknown_Release((IUnknown *)(p)); (p) = NULL; } while (0)

TEST(squeeze_shaders_compile)
{
    mt_begin();
    log_to_file();
    REQUIRE_EQ_INT(compile_shaders(), 1);
    CHECK(g.vs != NULL && g.ps != NULL);
    CHECK(log_has("squeeze shaders compiled"));
    RELEASE(g.vs);
    RELEASE(g.ps);
}

TEST(shader_compiler_errors_are_logged)
{
    mt_begin();
    log_to_file();
    HMODULE dc = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    D3DCompileFn compile = dc ? (D3DCompileFn)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    REQUIRE(compile != NULL);
    ID3DBlob *code = NULL;
    CHECK(FAILED(compile_entry(compile, "no_such_entry", "ps_5_0", &code)));
    CHECK(code == NULL);
    CHECK(log_has("shader compiler: "));
}

// ===================================================================================== the squeeze on a GPU (WARP)

typedef HRESULT(WINAPI *CreateDxgiFactoryFn)(REFIID iid, void **factory);
typedef HRESULT(WINAPI *CreateD3d12DeviceFn)(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID iid, void **device);

// The test's own D3D12 objects, for setting up and reading back the textures the squeeze works on.
static struct {
    IDXGIFactory4 *factory;
    ID3D12Device *dev;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    UINT64 value;
    HANDLE event;
    ID3D12DescriptorHeap *rtvs;
} G;

// Releases the squeeze's D3D12 objects (once the GPU is done with them), then the test's.
static void gpu_close(void *unused)
{
    if (g.fence && g.fenceEvent) wait_fence(g.fenceValue, 5000);
    RELEASE(g.pso);
    RELEASE(g.rootSig);
    RELEASE(g.rtvHeap);
    RELEASE(g.srvHeap);
    RELEASE(g.copy);
    for (int i = 0; i < RING; i++) RELEASE(g.alloc[i]);
    RELEASE(g.list);
    RELEASE(g.fence);
    if (g.fenceEvent) CloseHandle(g.fenceEvent);
    g.fenceEvent = NULL;
    RELEASE(g.queue);
    RELEASE(g.device);
    RELEASE(g.vs);
    RELEASE(g.ps);
    RELEASE(G.rtvs);
    RELEASE(G.list);
    RELEASE(G.alloc);
    RELEASE(G.fence);
    if (G.event) CloseHandle(G.event);
    RELEASE(G.queue);
    RELEASE(G.dev);
    RELEASE(G.factory);
    memset(&G, 0, sizeof(G));
}

// A WARP device with a direct queue, or the test is skipped where there's no D3D12 (Wine).
static void gpu_open(void)
{
    memset(&G, 0, sizeof(G));
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll"), d3d12 = LoadLibraryW(L"d3d12.dll");
    CreateDxgiFactoryFn createFactory = dxgi ? (CreateDxgiFactoryFn)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    CreateD3d12DeviceFn createDevice = d3d12 ? (CreateD3d12DeviceFn)(void *)GetProcAddress(d3d12, "D3D12CreateDevice") : NULL;
    if (!createFactory || !createDevice) SKIP("no dxgi.dll or d3d12.dll here");
    tt_defer(gpu_close, NULL);
    IDXGIAdapter *warp = NULL;
    HRESULT hr = createFactory(&IID_IDXGIFactory4, (void **)&G.factory);
    if (SUCCEEDED(hr)) hr = IDXGIFactory4_EnumWarpAdapter(G.factory, &IID_IDXGIAdapter, (void **)&warp);
    if (SUCCEEDED(hr)) hr = createDevice((IUnknown *)warp, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&G.dev);
    RELEASE(warp);
    if (FAILED(hr)) SKIP("no D3D12 WARP device here (hr=0x%08lX)", (unsigned long)hr);
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    REQUIRE(SUCCEEDED(ID3D12Device_CreateCommandQueue(G.dev, &qd, &IID_ID3D12CommandQueue, (void **)&G.queue)));
    REQUIRE(SUCCEEDED(ID3D12Device_CreateCommandAllocator(G.dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                                          (void **)&G.alloc)));
    REQUIRE(SUCCEEDED(ID3D12Device_CreateCommandList(G.dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, G.alloc, NULL,
                                                     &IID_ID3D12GraphicsCommandList, (void **)&G.list)));
    REQUIRE(SUCCEEDED(ID3D12GraphicsCommandList_Close(G.list)));
    REQUIRE(SUCCEEDED(ID3D12Device_CreateFence(G.dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&G.fence)));
    G.event = CreateEventW(NULL, FALSE, FALSE, NULL);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    REQUIRE(SUCCEEDED(ID3D12Device_CreateDescriptorHeap(G.dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&G.rtvs)));
}

// The squeeze set up on the test's queue, as it would be on the game's.
static void squeeze_ready(void)
{
    REQUIRE_EQ_INT(compile_shaders(), 1);
    ID3D12CommandQueue_AddRef(G.queue); // init_d3d_on takes a reference
    REQUIRE_EQ_INT(init_d3d_on(G.queue), 1);
}

static void gpu_begin(void)
{
    REQUIRE(SUCCEEDED(ID3D12CommandAllocator_Reset(G.alloc)));
    REQUIRE(SUCCEEDED(ID3D12GraphicsCommandList_Reset(G.list, G.alloc, NULL)));
}

// Runs the test's command list after everything already on the queue, and waits for it.
static void gpu_finish(void)
{
    REQUIRE(SUCCEEDED(ID3D12GraphicsCommandList_Close(G.list)));
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)G.list };
    ID3D12CommandQueue_ExecuteCommandLists(G.queue, 1, lists);
    REQUIRE(SUCCEEDED(ID3D12CommandQueue_Signal(G.queue, G.fence, ++G.value)));
    REQUIRE(SUCCEEDED(ID3D12Fence_SetEventOnCompletion(G.fence, G.value, G.event)));
    REQUIRE_MSG(WaitForSingleObject(G.event, 10000) == WAIT_OBJECT_0, "the GPU didn't finish in 10 s");
}

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b;
    barrier(&b, r, from, to);
    ID3D12GraphicsCommandList_ResourceBarrier(G.list, 1, &b);
}

// A texture like a back buffer, in the PRESENT state.
static ID3D12Resource *back_buffer_texture(UINT w, UINT h, DXGI_FORMAT format)
{
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC d;
    memset(&d, 0, sizeof(d));
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *r = NULL;
    REQUIRE(SUCCEEDED(ID3D12Device_CreateCommittedResource(G.dev, &heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PRESENT,
                                                           NULL, &IID_ID3D12Resource, (void **)&r)));
    tt_defer(release_deferred, r);
    return r;
}

// Four colours, two texel columns each: A A B B C C D D in an 8-wide texture.
static const float m_colours[4][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 } };

static void paint_pairs(ID3D12Resource *tex)
{
    D3D12_RESOURCE_DESC d = ID3D12Resource_GetDesc(tex);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(G.rtvs);
    ID3D12Device_CreateRenderTargetView(G.dev, tex, NULL, rtv);
    gpu_begin();
    transition(tex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (UINT x = 0; x < d.Width; x += 2) {
        D3D12_RECT r = { (LONG)x, 0, (LONG)x + 2, (LONG)d.Height };
        ID3D12GraphicsCommandList_ClearRenderTargetView(G.list, rtv, m_colours[(x / 2) % 4], 1, &r);
    }
    transition(tex, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    gpu_finish();
}

// The texels of a PRESENT-state texture of 4-byte texels, at most 64 wide, row after row.
static void read_texels(ID3D12Resource *tex, uint32_t *out)
{
    D3D12_RESOURCE_DESC td = ID3D12Resource_GetDesc(tex);
    REQUIRE(td.Width <= 64);
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC d;
    memset(&d, 0, sizeof(d));
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 256 * td.Height;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *buf = NULL;
    REQUIRE(SUCCEEDED(ID3D12Device_CreateCommittedResource(G.dev, &heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           NULL, &IID_ID3D12Resource, (void **)&buf)));
    tt_defer(release_deferred, buf);
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    memset(&dst, 0, sizeof(dst));
    memset(&src, 0, sizeof(src));
    dst.pResource = buf;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = td.Format;
    dst.PlacedFootprint.Footprint.Width = (UINT)td.Width;
    dst.PlacedFootprint.Footprint.Height = td.Height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu_begin();
    transition(tex, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyTextureRegion(G.list, &dst, 0, 0, 0, &src, NULL);
    transition(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    gpu_finish();
    uint8_t *p = NULL;
    D3D12_RANGE all = { 0, (SIZE_T)d.Width }, none = { 0, 0 };
    REQUIRE(SUCCEEDED(ID3D12Resource_Map(buf, 0, &all, (void **)&p)));
    for (UINT y = 0; y < td.Height; y++) memcpy(out + y * td.Width, p + y * 256, (size_t)td.Width * 4);
    ID3D12Resource_Unmap(buf, 0, &none);
}

// Each half of a squeezed frame is the whole frame at half the width. Sampling between the two texels of a
// pair, both halves of A A B B C C D D read A B C D.
static void check_squeezed(const uint32_t *before, const uint32_t *after, UINT w, UINT h)
{
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++) {
            uint32_t want = before[y * w + 2 * (x % (w / 2))];
            CHECK_MSG(after[y * w + x] == want, "texel (%u, %u) is 0x%08x, want 0x%08x", x, y, after[y * w + x], want);
        }
}

// Paints bb, squeezes it and checks every texel.
static void squeeze_and_check(ID3D12Resource *bb)
{
    D3D12_RESOURCE_DESC d = ID3D12Resource_GetDesc(bb);
    uint32_t before[64 * 2], after[64 * 2];
    REQUIRE(d.Width <= 64 && d.Height <= 2);
    paint_pairs(bb);
    read_texels(bb, before);
    REQUIRE_MSG(before[0] != before[2], "the test colours didn't paint");
    REQUIRE_EQ_INT(squeeze_back_buffer(bb), 1);
    read_texels(bb, after);
    check_squeezed(before, after, (UINT)d.Width, d.Height);
}

TEST(init_d3d_on_sets_up_everything_the_squeeze_records_with)
{
    mt_begin();
    log_to_file();
    gpu_open();
    squeeze_ready();
    CHECK_EQ_PTR(g.device, G.dev);
    CHECK_EQ_PTR(g.queue, G.queue);
    CHECK(g.rootSig && g.rtvHeap && g.srvHeap && g.list && g.fence && g.fenceEvent);
    for (int i = 0; i < RING; i++) CHECK_MSG(g.alloc[i] != NULL, "command allocator %d", i);
    CHECK(log_has("D3D12 ready: device "));
    CHECK_EQ_INT(g.failed, 0);
}

TEST(init_d3d_on_needs_the_shaders)
{
    mt_begin();
    log_to_file();
    gpu_open();
    ID3D12CommandQueue_AddRef(G.queue);
    CHECK_EQ_INT(init_d3d_on(G.queue), 0);
    CHECK_EQ_INT(g.failed, 1);
    CHECK(log_has("the squeeze shaders weren't compiled"));
}

TEST(squeeze_draws_the_whole_frame_into_each_half)
{
    mt_begin();
    log_to_file();
    gpu_open();
    squeeze_ready();
    squeeze_and_check(back_buffer_texture(8, 2, DXGI_FORMAT_R8G8B8A8_UNORM));
    CHECK_EQ_INT((int)g.squeezed, 1);
    CHECK_EQ_INT(g.failed, 0);
    CHECK(log_has("copy texture 8x2, DXGI format 28"));
    CHECK(log_has("squeeze pipeline created for DXGI format 28"));
}

TEST(squeeze_reuses_its_objects_and_remakes_them_for_a_new_size_or_format)
{
    mt_begin();
    log_to_file();
    gpu_open();
    squeeze_ready();
    ID3D12Resource *bb = back_buffer_texture(8, 2, DXGI_FORMAT_R8G8B8A8_UNORM);
    squeeze_and_check(bb);
    ID3D12Resource *copy = g.copy;
    ID3D12PipelineState *pso = g.pso;
    squeeze_and_check(bb);
    CHECK_EQ_PTR(g.copy, copy);
    CHECK_EQ_PTR(g.pso, pso);
    squeeze_and_check(back_buffer_texture(16, 2, DXGI_FORMAT_R8G8B8A8_UNORM)); // a new size: a new copy
    CHECK_EQ_INT((int)g.copyW, 16);
    CHECK_EQ_PTR(g.pso, pso);
    squeeze_and_check(back_buffer_texture(16, 2, DXGI_FORMAT_B8G8R8A8_UNORM)); // a new format: both new
    CHECK_EQ_INT(g.copyFormat, DXGI_FORMAT_B8G8R8A8_UNORM);
    CHECK_EQ_INT(g.psoFormat, DXGI_FORMAT_B8G8R8A8_UNORM);
    CHECK(log_has("copy texture 16x2, DXGI format 87"));
    CHECK(log_has("squeeze pipeline created for DXGI format 87"));
    CHECK_EQ_INT((int)g.squeezed, 4);
    CHECK_EQ_INT(g.failed, 0);
}

TEST(squeezes_go_round_the_command_allocators)
{
    mt_begin();
    log_to_file();
    gpu_open();
    squeeze_ready();
    ID3D12Resource *bb = back_buffer_texture(8, 2, DXGI_FORMAT_R8G8B8A8_UNORM);
    for (int i = 0; i < RING + 2; i++) REQUIRE_EQ_INT(squeeze_back_buffer(bb), 1);
    CHECK_EQ_INT(g.ring, (RING + 2) % RING);
    CHECK_EQ_INT((int)g.fenceValue, RING + 2);
    CHECK_EQ_INT((int)g.allocFence[1], RING + 2); // slot 1 was used again by the last one
    CHECK_EQ_INT((int)g.squeezed, RING + 2);
}

TEST(wait_fence_gives_up_after_its_time)
{
    mt_begin();
    log_to_file();
    gpu_open();
    squeeze_ready();
    CHECK_EQ_INT(wait_fence(0, 0), 1);
    DWORD start = GetTickCount();
    CHECK_EQ_INT(wait_fence(5, 50), 0);
    CHECK_MSG(GetTickCount() - start >= 40, "gave up after %lu ms", GetTickCount() - start);
    REQUIRE(SUCCEEDED(ID3D12Fence_Signal(g.fence, 5)));
    CHECK_EQ_INT(wait_fence(5, 50), 1);
}

// A swap chain of 8x2 back buffers on the test's queue, for a window.
static IDXGISwapChain *test_swap_chain(void)
{
    HWND hwnd = tu_window(L"squeeze test", 0, 0, 64, 64);
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof(sd));
    sd.Width = 8;
    sd.Height = 2;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForHwnd(G.factory, (IUnknown *)G.queue, hwnd, &sd, NULL, NULL, &sc);
    REQUIRE_MSG(SUCCEEDED(hr), "CreateSwapChainForHwnd failed (hr=0x%08lX)", (unsigned long)hr);
    tt_defer(release_deferred, sc);
    return (IDXGISwapChain *)sc;
}

TEST(squeeze_squeezes_the_swap_chains_current_back_buffer)
{
    mt_begin();
    log_to_file();
    gpu_open();
    squeeze_ready();
    IDXGISwapChain *sc = test_swap_chain();
    UINT index = 99;
    ID3D12Resource *bb = current_back_buffer(sc, &index);
    REQUIRE(bb != NULL);
    tt_defer(release_deferred, bb);
    CHECK_EQ_INT(index, 0);
    uint32_t before[16], after[16];
    paint_pairs(bb);
    read_texels(bb, before);
    squeeze(sc);
    read_texels(bb, after);
    check_squeezed(before, after, 8, 2);
    CHECK(log_has("first squeeze: back buffer 0, 8x2"));
    CHECK_EQ_INT(g.failed, 0);
}

TEST(back_buffers_of_another_device_are_never_squeezed)
{
    mt_begin();
    log_to_file();
    gpu_open();
    IDXGISwapChain *sc = test_swap_chain();
    g.device = (ID3D12Device *)&m_ddVar; // any other device
    UINT index;
    CHECK(current_back_buffer(sc, &index) == NULL);
    CHECK_EQ_INT(g.failed, 1);
    CHECK(log_has("the back buffer belongs to another D3D12 device"));
    g.device = NULL;
}

int main(int argc, char **argv)
{
    tt_set_args(argc, argv);
    return tt_main(argc, argv);
}
