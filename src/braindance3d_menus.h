// 3D Braindance Edition: the RED4ext plugin's hooks (braindance3d_menus.c, braindance3d_framegen.c).
#pragma once
#include <stdbool.h>
#include <wchar.h>

// RED4ext SDK v1 (include/RED4ext/Api/v1/Sdk.hpp, Hooking.hpp): the parts this plugin uses.
typedef struct {
    bool (*Attach)(void *handle, void *target, void *detour, void **original);
    bool (*Detach)(void *handle, void *target);
} R4eHooking;
typedef struct {
    void *runtime;
    void *logger;
    R4eHooking *hooking;
    void *gameStates;
    void *scripts;
} R4eSdk;

// Installs the menu squeeze. Call from the plugin's Main on Load. Returns 1 when it is set up (or off by
// setting), 0 when it couldn't be; the reason is in Braindance3D-hooks.log. Never stops the game.
// It also reads [Log] Level and opens that log, so call it before framegen_load.
int menus_load(void *pluginHandle, const R4eSdk *sdk, const wchar_t *installDir);

// Tells the menu squeeze which way the game's 3D Mode handler just set 3D Mode (from braindance3d_framegen.c).
void menus_note_3d_mode(int on);

// Shows FIXED 3D ON or FIXED 3D OFF on the picture for two seconds, in both eyes.
void menus_notice(int on);

// Writes a line to Braindance3D-hooks.log when [Log] Level is at least level (1 normal, 2 verbose).
void plugin_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Installs the frame generation hooks (DLSS frame generation in 3D Mode), and the weapon depth hook on its own. Call
// from Main on Load, after menus_load. Returns 1 when the frame generation hooks are attached, 0 when not (the reason
// is logged). Never stops the game.
int framegen_load(void *pluginHandle, const R4eSdk *sdk);

// The picture 3D Mode sends out ([Display] Output): side by side (the mod's own), or the game's stock output for a
// glasses-free 3D monitor, whose own SDK weaves the two eyes. Side by side is half SBS (each eye squeezed into its half,
// the game's own way) or full SBS (each eye a whole undistorted picture in its half of a window two eyes wide, such as
// 3840x1080 for XR glasses with 1920x1080 per eye). OUTPUT_SBS picks by the window: full SBS from 3:1 (FULL_SBS_AUTO).
// OUTPUT_FULL_SBS is full SBS whenever the window is at least two eyes of 1:1 wide (FULL_SBS_FORCED), so glasses
// switched to 2D (1920x1080) still get half SBS.
enum { OUTPUT_SBS = 0, OUTPUT_MONITOR = 1, OUTPUT_HALF_SBS = 2, OUTPUT_FULL_SBS = 3 };
#define FULL_SBS_AUTO 3.0f
#define FULL_SBS_FORCED 2.0f
// Mod Settings' file, from the mod's folder (bin\x64\Braindance3D, three levels under the game), and our section in it:
// the class in FixedStereo.reds.
#define MOD_SETTINGS_FROM_MOD_DIR L"\\..\\..\\..\\red4ext\\plugins\\mod_settings\\user.ini"
#define MOD_SETTINGS_SECTION L"BraindanceFixedStereo"

// Sets it, from the auto-setup's result. With OUTPUT_MONITOR the 3D Mode handler hook turns [Odyssey3D] SBSDebug off
// (the engine ini turns it on), so the game uses its stock path and every SBSDebug-gated change steps aside. Call before
// framegen_load.
void framegen_set_output(int output);

// The output Mod Settings' Output = Auto stands for (the ini's [Display] Output, or the auto-setup's pick): Output
// switches between Half SBS, Full SBS and Auto live, when Mod Settings saves it (except to or from a 3D monitor's own
// output, which the game decides at start). Call before stereo_settings_load.
void framegen_set_output_auto(int output);

// Whether 3D Mode draws side by side: [Odyssey3D] SBSDebug is on, or the var wasn't found (as before this check).
int framegen_side_by_side(void);

// Whether a frame this size is drawn full side by side now: the output allows it, its hooks are attached, SBSDebug is
// on and the window is wide enough. The menu squeeze then shows the middle half of a flat frame 1:1 in each half
// (where the game puts menus in so wide a window) instead of the whole frame squeezed.
int framegen_full_sbs(unsigned width, unsigned height);

// Reads the Fixed 3D settings from installDir and Mod Settings' file. Call before framegen_load.
void stereo_settings_load(const wchar_t *installDir);
