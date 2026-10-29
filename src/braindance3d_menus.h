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

// Writes a line to Braindance3D-hooks.log when [Log] Level is at least level (1 normal, 2 verbose).
void plugin_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Installs the frame generation hooks (DLSS frame generation in 3D Mode). Call from Main on Load, after
// menus_load. Returns 1 when they are attached, 0 when not (the reason is logged). Never stops the game.
int framegen_load(void *pluginHandle, const R4eSdk *sdk);
