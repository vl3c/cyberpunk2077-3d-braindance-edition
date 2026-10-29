# 3D Braindance Edition Mod for Cyberpunk 2077

![3D Braindance Edition](docs/promo.jpg)

Play Cyberpunk 2077 in stereoscopic 3D using the game's own 3D renderer, in any 3D / VR headset with
side-by-side (SBS) support, where the game appears on a big virtual screen with real depth. The game draws
half-SBS; on a 4K screen that gives each eye 1920x2160 (see [Recommended setup](#recommended-setup-quest-3--virtual-desktop)).
Tested with a Quest 3 and Virtual Desktop; other headsets, 3D TVs and AR glasses with a half-SBS mode should
work but are untested.

**Not VR: no head tracking, no motion controls!** You look at a 3D screen.

Download: [Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/36323) or
[GitHub releases](https://github.com/vl3c/cyberpunk2077-3d-braindance-edition/releases).

## What it is

Patch 2.5 added a 3D Mode for Samsung's Odyssey 3D monitor. The game renders a separate image for each eye,
with real depth for the world, the HUD and your weapon. It only switches that on when Samsung's Odyssey 3D
software is installed, so for everyone else the option is greyed out.

This mod does two things:

1. It provides a small stand-in for Samsung's Odyssey 3D SDK. The game finds it and unlocks 3D Mode.
2. It turns on a hidden engine option (`SBSDebug`), so the game draws the left and right eye side by side
   instead of interlacing them for Samsung's lenticular panel.

It doesn't modify any game file and doesn't hook or patch the game's memory. The game loads the stand-in DLL itself, through its own Odyssey 3D code path.

## Requirements

- Cyberpunk 2077 **2.5** on Windows 10 or 11. Tested on 2.5 from the Epic Games Store. Steam and GOG should
  behave the same but are untested. A later patch may change what the game expects; see Troubleshooting.
- [RED4ext](https://www.nexusmods.com/cyberpunk2077/mods/2380), which sets the mod up automatically when the
  game starts. Without RED4ext, run the setup script once instead (see below).
- A 3D / VR headset with half-SBS support (a 3D TV or monitor with an SBS mode works too). Tested: Quest 3 with
  Virtual Desktop.
- 3D Mode needs the game's own Frame Generation (DLSS, FSR or XeSS) off: the two can't be on together. You can
  add frame generation with OptiScaler instead (see Recommended setup).

## Install

1. Install the zip with Vortex, or extract it into your game folder (the one with `bin`, `engine` and
   `red4ext` in it).
2. Start the game, open the video settings, and turn on **3D Mode**. The screen now shows two images side by side.
3. In your headset, show the PC screen in half side-by-side 3D. In Virtual Desktop, set the desktop
   screen's 3D mode to half side-by-side (Half SBS). The game must run on the monitor the headset shows.

The game finds the 3D SDK through one per-user registry key, `HKCU\SOFTWARE\Samsung\Odyssey3DHub`. With
RED4ext, the mod's plugin (`red4ext\plugins\Braindance3D`) writes it at every game start, so there's nothing
to run. It logs what it did to `bin\x64\Braindance3D\Braindance3D-autosetup.log`.

**Without RED4ext:** use the full zip from [GitHub releases](https://github.com/vl3c/cyberpunk2077-3d-braindance-edition/releases)
(the Nexus download leaves the setup scripts out). Double-click `bin\x64\Braindance3D\Install-Braindance3D.bat`
once, not as administrator. It writes the same key, needs no admin rights, and has to be run again only if you
move the game.

Both leave Samsung's own Odyssey 3D Hub alone if it's installed. Mod Organizer 2 isn't supported yet: the game
must find the DLL in a real folder, and the engine ini only exists inside MO2's virtual file system.

## Settings

- **Depth** sets the strength of the 3D effect. Values from 0.5 to 1.0 work well in a headset.
- **Weapon Depth** and **Motion Comfort** tune your gun and how strong the effect is while moving; **Preset**
  picks a set of these.
- Each eye gets half the horizontal resolution (half-SBS), so a higher game resolution looks sharper. A 4K
  screen is recommended.
- The game renders every frame twice, so expect a lower frame rate than in 2D.
- To play in 2D again, turn 3D Mode off, or uninstall.
- Stereo 3D can cause eye strain or discomfort. Take breaks, and lower Depth if your eyes get tired.

## Recommended setup (Quest 3 + Virtual Desktop)

This is what we play with: an RTX 5080 (16 GB), path tracing on, a Quest 3 and Virtual Desktop. Other setups
work too; this one gives the sharpest and smoothest picture we've found.

1. **Play on a 4K screen.** Each eye gets half the width of the game window, so the window size decides the
   sharpness. Create a 3840x2160 virtual monitor (for example with a virtual display driver), make it the
   primary monitor, and run the game borderless at 3840x2160 on it. Each eye then gets 1920x2160. Virtual
   Desktop streams at most 3840x2160 per screen, so going higher doesn't help. NVIDIA DLDSR 2.25x on a 1440p
   monitor is an alternative.
2. **Half SBS in Virtual Desktop.** Set the desktop screen's 3D mode to Half SBS.
3. **Match the refresh rates.** Set the virtual monitor's refresh rate to Virtual Desktop's streaming rate
   (both 72 Hz, or both 90 Hz). Frames above the streaming rate are dropped, and a mismatch causes judder.
4. **Path tracing with DLSS on Performance.** 3D renders every frame twice. At 4K, DLSS Performance (Balanced
   if you have headroom) keeps the base frame rate up. It renders the whole side-by-side frame at 1920x1080
   inside, so each eye starts from 960x1080 before DLSS scales it up.
5. **Frame generation through OptiScaler.** The game's own frame generation can't be on in 3D Mode, but
   [OptiScaler](https://github.com/optiscaler/OptiScaler) can add it on top. The `dlssg` output used here is
   only in OptiScaler's development (nightly) builds so far; the 0.9.x releases don't have it.
   - Install OptiScaler as `winhttp.dll` in `bin\x64` (what we tested). Don't use `winmm.dll` or `version.dll`:
     RED4ext and Cyber Engine Tweaks already use those names, and without RED4ext 3D Mode stays greyed out.
   - Leave the game's Frame Generation off, and pick DLSS as the upscaler in game, so OptiScaler can take it
     over (`FGInput = upscaler` needs that).
   - In `OptiScaler.ini`, `[FrameGen]`: `Enabled = true`, `FGInput = upscaler`, `FGOutput = dlssg` (NVIDIA DLSS
     frame generation). `[OptiFG]`: `HUDFix = true`.
   - DLSS-G output needs Streamline next to OptiScaler: copy the game's own `sl.interposer.dll`,
     `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll` and `nvngx_dlssg.dll` from `bin\x64` into
     `bin\x64\OptiScaler\streamline`.
   - In game, End toggles frame generation and Insert opens OptiScaler's menu.

   Tested with an OptiScaler development build (it reports 10.0.0-dev) on an RTX 5080, stable up to 6x. The
   setting names can differ between builds.
6. **Pick the multiplier from the refresh rate.** The refresh rate divided by the multiplier is the base frame
   rate the game has to reach. Input lag follows the base rate: we find 24 or more playable for a 3D screen
   you look at, but many players want 40 or more. RTX 40 cards stop at 2x; 3x and higher, and the automatic
   multiplier, need an RTX 50 card. Examples:
   - RTX 50: 72 Hz with 3x needs a base of 24; 90 Hz with 3x needs 30; 120 Hz with 4x needs 30.
   - RTX 40: 72 Hz with 2x needs 36; 90 Hz with 2x needs 45.
7. **Watch the video memory.** 4K, path tracing and a high multiplier can fill a 16 GB card. If the base frame
   rate suddenly collapses (and `OptiScaler.log` shows `eWarnOutOfVRAM`), lower texture quality or the
   multiplier.

## Uninstall

Remove the mod in Vortex, or delete its files (including `red4ext\plugins\Braindance3D`). The registry key
stays behind and points at a folder that is gone, so the game just stops finding the stand-in. To remove the key
too, run `Uninstall-Braindance3D.bat` (full zip) after removing the RED4ext plugin, or delete
`HKCU\SOFTWARE\Samsung\Odyssey3DHub` in regedit if nothing else uses it. With RED4ext, the uninstaller alone
isn't enough: the plugin registers the mod again at the next game start.

**If you own a real Odyssey 3D monitor, don't install this mod.** The engine ini alone makes the game draw side
by side instead of using your monitor's 3D, even if you never run the installer. The installer won't replace
Samsung's software (neither does the RED4ext plugin) unless you run `Install-Braindance3D.bat -Force` from a
command prompt. If you do, run the uninstaller before deleting the mod: it restores Samsung's setting.

## Troubleshooting

- **The game crashes or won't start, after installing or after a game update:** remove the mod in Vortex (or
  delete its files, including `red4ext\plugins\Braindance3D`). The game then starts normally. Please report it
  with your game version.
- **3D Mode is still greyed out:** check `Braindance3D-autosetup.log` (RED4ext) or, without RED4ext, run the
  installer again and read the log it names at the end (`Braindance3D-setup.log`). Both are in `bin\x64\Braindance3D`. On a PC with several monitors, try `Monitor = primary` in
  `Braindance3D-Settings.ini`, and play on the primary monitor.
- **The mod's log:** `bin\x64\Braindance3D\braindance3d.log` (the previous run is `braindance3d.old.log`), or
  `%LOCALAPPDATA%\Braindance3D` if the game folder is read-only.
- **Menus look wrong in the headset:** a known issue. The game draws menus once across the whole screen
  instead of once per eye, so in half-SBS each eye sees a different half of the menu.
- **The depth looks inside out:** use your viewer's swap-eyes option.
- **Other games:** the registry key isn't per game. The stand-in refuses to work in anything but
  `Cyberpunk2077.exe`, but if another game with Odyssey 3D support misbehaves, uninstall before playing it.

**Reporting bugs:** open an issue at
https://github.com/vl3c/cyberpunk2077-3d-braindance-edition/issues or use the Bugs tab on Nexus. Include your
game version and store, GPU, how you view the 3D, `Braindance3D-autosetup.log` or `Braindance3D-setup.log`, and `braindance3d.log` with
`Level = 2` set in `Braindance3D-Settings.ini`. The logs contain folder paths, which can include your Windows
user name.

## How it works and building

See [docs/TECHNICAL.md](https://github.com/vl3c/cyberpunk2077-3d-braindance-edition/blob/main/docs/TECHNICAL.md).
To build: `./build.sh` (MinGW-w64), or `./build.sh dist` for the release zip. Tests: see `tests/README.md`.

## Legal

This is an unofficial fan work and is not approved or endorsed by CD PROJEKT RED. It is not affiliated with,
endorsed by or supported by Samsung. Use it at your own risk: it changes how the game renders. The
`Odyssey3DSDK.dll` in this mod is not Samsung's SDK. It is an independent program written from scratch, and its
interface was worked out from how the game calls it, solely so that it can work with the game. It contains no
code, headers or binaries from CD PROJEKT RED or Samsung.

CD PROJEKT®, Cyberpunk® and Cyberpunk 2077® are registered trademarks of CD PROJEKT S.A. Samsung, Odyssey and
Odyssey 3D are trademarks of Samsung Electronics Co., Ltd. Meta Quest is a trademark of Meta Platforms, Inc.

Released under the [MIT License](LICENSE).
