# 3D Braindance Edition Mod for Cyberpunk 2077

![3D Braindance Edition](docs/promo.jpg)

Play Cyberpunk 2077 in stereoscopic 3D using the game's own 3D renderer, in any 3D / VR headset with
side-by-side (SBS) support, where the game appears on a big virtual screen with real depth. Half-SBS for now,
full SBS is planned. Tested with a Quest 3 and Virtual Desktop; other headsets, 3D TVs and AR glasses with a
half-SBS mode should work but are untested.

**Not VR: no head tracking, no motion controls!** You look at a 3D screen.

Download: [Nexus Mods](https://www.nexusmods.com/cyberpunk2077) or
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
- If 3D Mode stays greyed out, turn off Frame Generation (DLSS, FSR or XeSS). It was off in all our tests.

## Install

1. Install the zip with Vortex, or extract it into your game folder (the one with `bin`, `engine` and
   `red4ext` in it).
2. Start the game, open the video settings, and turn on **3D Mode**. The screen now shows two images side by side.
3. In your headset, show the PC screen in half side-by-side 3D. In Virtual Desktop, set the desktop
   screen's 3D mode to half side-by-side (Half SBS). The game must run on the monitor the headset shows.

The game finds the 3D SDK through one per-user registry key, `HKCU\SOFTWARE\Samsung\Odyssey3DHub`. With
RED4ext, the mod's plugin (`red4ext\plugins\Braindance3D`) writes it at every game start, so there's nothing
to run. It logs what it did to `bin\x64\Braindance3D\Braindance3D-autosetup.log`.

**Without RED4ext:** double-click `bin\x64\Braindance3D\Install-Braindance3D.bat` once, not as administrator.
It writes the same key, needs no admin rights, and has to be run again only if you move the game.

Both leave Samsung's own Odyssey 3D Hub alone if it's installed. Mod Organizer 2 isn't supported yet: the game
must find the DLL in a real folder, and the engine ini only exists inside MO2's virtual file system.

## Settings

- **Depth** sets the strength of the 3D effect. Values from 0.5 to 1.0 work well in a headset.
- **Weapon Depth** and **Motion Comfort** tune your gun and how strong the effect is while moving; **Preset**
  picks a set of these.
- Each eye gets half the horizontal resolution (half-SBS; full SBS is planned), so a higher game resolution looks sharper.
- The game renders every frame twice, so expect a lower frame rate than in 2D.
- To play in 2D again, turn 3D Mode off, or uninstall.
- Stereo 3D can cause eye strain or discomfort. Take breaks, and lower Depth if your eyes get tired.

## Uninstall

Remove the mod in Vortex, or delete its files. The registry key stays behind and does no harm: the game just
stops finding the stand-in. To remove the key too, double-click `bin\x64\Braindance3D\Uninstall-Braindance3D.bat`
before deleting the files.

**If you own a real Odyssey 3D monitor, don't install this mod.** The engine ini alone makes the game draw side
by side instead of using your monitor's 3D, even if you never run the installer. The installer won't replace
Samsung's software (neither does the RED4ext plugin) unless you run `Install-Braindance3D.bat -Force` from a
command prompt. If you do, run the uninstaller before deleting the mod: it restores Samsung's setting.

## Troubleshooting

- **The game crashes or won't start, after installing or after a game update:** remove the mod (or run
  `Uninstall-Braindance3D.bat`). The game then starts normally. Please report it with your game version.
- **3D Mode is still greyed out:** check `Braindance3D-autosetup.log` (RED4ext) or run the installer again and
  read the log it names at the end (`Braindance3D-setup.log`). Both are in `bin\x64\Braindance3D`. On a PC with several monitors, try `Monitor = primary` in
  `Braindance3D-Settings.ini`, and play on the primary monitor.
- **The mod's log:** `bin\x64\Braindance3D\braindance3d.log` (the previous run is `braindance3d.old.log`), or
  `%LOCALAPPDATA%\Braindance3D` if the game folder is read-only.
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
