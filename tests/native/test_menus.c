// White-box tests for the menu squeeze: this file #includes src/braindance3d_menus.c so it can drive its
// detection state machine, the UserSettings.json seed and the settings reader without the
// game or a GPU. The hook and the D3D12 squeeze itself are only tested in game.
//
// Run: test_menus.exe [--list] [name filters...]

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
    if (!once) {
        InitializeCriticalSection(&g.logLock);
        once = 1;
    }
    g.mode = MODE_AUTO;
    g.level = 0;
    g.log = NULL;
    g.failed = 0;
    g.deviceCalls = fake_device_calls;
    g.flatFrame = 0;
    g.stereo = STEREO_UNKNOWN;
    g.flatStreak = 0;
    g.gameFrames = 0;
}

// Frame patterns measured on the PC (2.5, 2026-10-29): what the game asks the stand-in per frame.
static void stereo_frame(void) { note_frame_calls(2, 2); }    // 3D gameplay or title
static void menu_frame(void) { note_frame_calls(0, 0); }      // inventory, ESC, loading
static void map_frame(void) { note_frame_calls(0, 2); }       // the map in 3D
static void flat_2d_frame(void) { note_frame_calls(0, 1); }   // 2D gameplay
static void frames(void (*f)(void), int n) { for (int i = 0; i < n; i++) f(); }

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

int main(int argc, char **argv)
{
    tt_set_args(argc, argv);
    return tt_main(argc, argv);
}
