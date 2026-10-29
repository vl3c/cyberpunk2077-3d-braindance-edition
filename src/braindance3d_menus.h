// 3D Braindance Edition: menu squeeze (see braindance3d_menus.c).
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
// setting), 0 when it couldn't be; the reason is in Braindance3D-menus.log. Never stops the game.
int menus_load(void *pluginHandle, const R4eSdk *sdk, const wchar_t *installDir);
