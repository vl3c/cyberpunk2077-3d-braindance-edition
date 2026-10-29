# 3D Braindance Edition Mod for Cyberpunk 2077

![3D Braindance Edition](docs/promo.jpg)

Play Cyberpunk 2077 in **real stereoscopic 3D** using the game's own 3D renderer, shown side by side for any
**3D display / VR headset with SBS support**. The game appears on a big virtual screen with real depth.

**Not VR: no head tracking, no motion controls!** You look at a 3D screen.

Download: [Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/36323) or
[GitHub releases](https://github.com/vl3c/cyberpunk2077-3d-braindance-edition/releases).

## What it does

Patch 2.5 added a 3D Mode made for a glasses-free 3D monitor. The game renders a separate image for each eye,
with real depth for the world, the HUD and your weapon, but only switches it on when the monitor's own software
is installed. For everyone else the option is greyed out. This mod:

1. Provides a small stand-in for that software (its 3D SDK), so the game unlocks **3D Mode** on a normal monitor.
2. Turns on a hidden engine option (`SBSDebug`), so the game draws the eyes **side by side** instead of
   interlacing them for the monitor's lenticular panel.
3. Makes **menus readable** (with RED4ext): the game draws menus, the map and loading screens as one flat
   picture, so each eye would see a different half. The mod squeezes that picture into each half.

View that in half-SBS and you see the game in 3D. It's the game's own stereo renderer, not a depth-map effect.
No game files are modified: the game loads the stand-in DLL through its own 3D support, and only the menu fix
uses a hook (the game's present, the same address Cyber Engine Tweaks uses).

## Requirements

- Cyberpunk 2077 **2.5** on Windows 10 or 11. Tested on 2.5 (Epic). Steam and GOG should behave the same but
  are untested.
- [RED4ext](https://www.nexusmods.com/cyberpunk2077/mods/2380). Without it, use the setup script from the
  GitHub download (see [No RED4ext](#no-red4ext)); menus then stay split.
- A 3D / VR headset with **half-SBS support** (a 3D TV or monitor with an SBS mode works too). Tested: Quest 3
  with Virtual Desktop.
- Own a glasses-free 3D monitor that the game supports? Don't install this mod: it switches the game to
  side-by-side output.

## Install

1. Install [RED4ext](https://www.nexusmods.com/cyberpunk2077/mods/2380) if you don't have it yet.
2. Install this mod with Vortex, or extract it into the game folder (the one with `bin`, `engine` and `red4ext`).
3. Start the game and turn on **3D Mode** in the video settings (this turns the game's own Frame Generation
   off). The screen now shows two images side by side.
4. In your headset, show the PC screen in **half side-by-side** 3D (in Virtual Desktop: the desktop screen's 3D
   mode, Half SBS). The game must run on the monitor the headset shows.

The mod sets itself up at every game start. To play in 2D, turn 3D Mode off.

## Settings

- In game: **Depth** (0.5 to 1.0 works well in a headset), **Weapon Depth**, **Motion Comfort** and **Preset**.
- `bin\x64\Braindance3D\Braindance3D-Settings.ini`: log level, which monitor counts as the 3D one
  (`Monitor = primary` for multi-monitor PCs) and the menu fix (`[Menus] Squeeze = auto` or `off`).
  **Ctrl+F8** flips the menu fix by hand for the screen that is showing.
- Each eye gets half the horizontal resolution, so **a higher game resolution looks sharper**. The game renders
  every frame twice, so expect a lower frame rate than in 2D.
- Stereo 3D can cause eye strain: take breaks, and lower Depth if your eyes get tired.

## Recommended setup (Quest 3 + Virtual Desktop)

What we play with: an RTX 5080 (16 GB), path tracing, a Quest 3 and Virtual Desktop.

1. **4K screen.** Create a 3840x2160 virtual monitor (a virtual display driver), make it primary, and run the
   game borderless at 3840x2160 on it: each eye gets 1920x2160. Virtual Desktop streams at most 3840x2160 per
   screen, so higher doesn't help. NVIDIA DLDSR 2.25x on a 1440p monitor is an alternative.
2. **Half SBS** for the desktop screen in Virtual Desktop.
3. **Matching refresh rates:** the virtual monitor at Virtual Desktop's streaming rate (both 72 Hz or both 90 Hz).
   Faster frames are dropped, and a mismatch judders.
4. **Path tracing with DLSS Performance** (Balanced if you have headroom). DLSS renders the whole side-by-side
   frame at 1920x1080 inside, so each eye starts from 960x1080.
5. **Frame generation through [OptiScaler](https://github.com/optiscaler/OptiScaler).** The game's own frame
   generation can't run in 3D Mode, but OptiScaler can add it. The `dlssg` output used here is only in
   OptiScaler's development (nightly) builds so far, not in the 0.9.x releases.
   - Install OptiScaler as `winhttp.dll` in `bin\x64`. Not as `winmm.dll` or `version.dll`: RED4ext and Cyber
     Engine Tweaks use those names, and replacing `winmm.dll` stops RED4ext (and this mod's setup) loading.
   - Pick DLSS as the upscaler in game, so `FGInput = upscaler` can take it over.
   - `OptiScaler.ini`: `[FrameGen]` `Enabled = true`, `FGInput = upscaler`, `FGOutput = dlssg`; `[OptiFG]`
     `HUDFix = true`.
   - DLSS-G output needs Streamline next to OptiScaler: copy the game's `sl.interposer.dll`, `sl.common.dll`,
     `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll` and `nvngx_dlssg.dll` from `bin\x64` into
     `bin\x64\OptiScaler\streamline`.
   - In game, End toggles frame generation and Insert opens OptiScaler's menu.

   Tested with an OptiScaler development build (10.0.0-dev), stable up to 6x. Setting names can differ between
   builds.
6. **Multiplier from the refresh rate:** refresh rate / multiplier = the base frame rate the game must reach, and
   input lag follows the base rate. We find 24+ playable for a screen you look at; many want 40+. RTX 40 cards
   stop at 2x; 3x and up need an RTX 50.
   - RTX 50: 72 Hz 3x needs 24; 90 Hz 3x needs 30; 120 Hz 4x needs 30.
   - RTX 40: 72 Hz 2x needs 36; 90 Hz 2x needs 45.
7. **Video memory:** 4K, path tracing and a high multiplier can fill 16 GB. If the base rate collapses (and
   `OptiScaler.log` shows `eWarnOutOfVRAM`), lower texture quality or the multiplier.

## Troubleshooting

- **Crash or no start after installing or after a game update:** disable the mod and report it with your game
  version.
- **3D Mode greyed out:** check that RED4ext is installed and read `bin\x64\Braindance3D\Braindance3D-autosetup.log`.
  With several monitors, try `Monitor = primary` and play on the primary monitor.
- **Menus look wrong:** set `[Menus] Squeeze = off` and report it with `Braindance3D-menus.log`. OptiScaler's own
  menu and FPS counter are drawn after the menu fix and still look split; `FpsOverlayPos = 0` in
  `OptiScaler.ini` keeps the counter readable in the left eye.
- **Depth inside out:** use your viewer's swap-eyes option.
- **Logs:** `braindance3d.log`, `Braindance3D-autosetup.log` and `Braindance3D-menus.log` in
  `bin\x64\Braindance3D` (or `%LOCALAPPDATA%\Braindance3D` if the game folder is read-only).

**Bugs:** open an [issue](https://github.com/vl3c/cyberpunk2077-3d-braindance-edition/issues) or use the Bugs tab
on Nexus. Include your game version and store, GPU, how you view the 3D, and the logs with `Level = 2` in
`Braindance3D-Settings.ini`. The logs contain folder paths, which can include your Windows user name.

## No RED4ext, uninstall and other details

- **How it registers:** the game finds the SDK through one per-user registry key,
  `HKCU\SOFTWARE\Samsung\Odyssey3DHub`. The RED4ext plugin writes it at every game start. Neither it nor the
  script replaces the 3D monitor's own software if that is installed.
- <a id="no-red4ext"></a>**No RED4ext:** use the full zip from
  [GitHub releases](https://github.com/vl3c/cyberpunk2077-3d-braindance-edition/releases) (the Nexus download
  leaves the scripts out) and double-click `bin\x64\Braindance3D\Install-Braindance3D.bat` once, not as
  administrator. Run it again only if you move the game.
- **Uninstall:** remove the mod in Vortex (or delete its files, including `red4ext\plugins\Braindance3D`). The
  registry key stays but points at a missing folder, so the game just stops finding the stand-in. To remove it
  too, run `Uninstall-Braindance3D.bat` (full zip) after removing the RED4ext plugin.
- **Real 3D monitor owners:** the engine ini alone switches 3D Mode to side by side. If you ran
  `Install-Braindance3D.bat -Force`, run the uninstaller before deleting the mod: it restores the monitor
  software's setting.
- **Mod Organizer 2** isn't supported yet: the game must find the DLL in a real folder.
- **Other games:** the registry key isn't per game. The stand-in only works in `Cyberpunk2077.exe`, but if
  another game with Odyssey 3D support misbehaves, uninstall before playing it.

## How it works and building

See [docs/TECHNICAL.md](https://github.com/vl3c/cyberpunk2077-3d-braindance-edition/blob/main/docs/TECHNICAL.md).
To build: `./build.sh` (MinGW-w64), or `./build.sh dist` for the release zips. Tests: see `tests/README.md`.

## Legal

This is an unofficial fan work and is not approved or endorsed by CD PROJEKT RED. It is not affiliated with,
endorsed by or supported by Samsung. Use it at your own risk: it changes how the game renders. The
`Odyssey3DSDK.dll` in this mod is not Samsung's SDK. It is an independent program written from scratch, and its
interface was worked out from how the game calls it, solely so that it can work with the game. It contains no
code, headers or binaries from CD PROJEKT RED or Samsung.

CD PROJEKT®, Cyberpunk® and Cyberpunk 2077® are registered trademarks of CD PROJEKT S.A. Samsung, Odyssey and
Odyssey 3D are trademarks of Samsung Electronics Co., Ltd. Meta Quest is a trademark of Meta Platforms, Inc.

Released under the [MIT License](LICENSE).
