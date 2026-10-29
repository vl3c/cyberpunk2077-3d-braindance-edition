# Changelog

## 0.2.0 (in progress)

- Menus, the map and loading screens are readable in 3D: the flat picture is squeezed into each half of the
  side-by-side image, so both eyes see the same screen. Only while 3D Mode is on; Ctrl+F8 flips it by hand for
  the screen that is showing (needs RED4ext; `[Menus] Squeeze` in `Braindance3D-Settings.ini`). The RED4ext
  plugin now hooks the game's present to do this.
- Turning 3D Mode off in the game's settings now really turns side-by-side off (before, the mod's engine ini
  forced stereo on).
- Experimental `PanelSize` setting in `Braindance3D-Settings.ini` (`[Display]`): the 3D screen's physical size in cm,
  which the game may use to work out the depth. `default` keeps the previous behaviour; a value it can't read is
  logged and ignored.
- README: recommended setup for Quest 3 and Virtual Desktop (4K virtual monitor, matching refresh rates,
  DLSS Performance with path tracing, frame generation through OptiScaler), and a known issue for menus.

## 0.1.0 (2026-10-28)

First release.

- Odyssey 3D SDK stand-in that unlocks the game's 3D Mode on an ordinary monitor (by default the monitor the
  game window is on; `Monitor = primary` in `Braindance3D-Settings.ini` uses the primary monitor). It only
  works inside `Cyberpunk2077.exe`.
- Engine ini that makes 3D Mode draw both eyes side by side (half-SBS).
- RED4ext plugin that sets the mod up automatically at every game start, so mod-manager installs need no
  extra step. It never replaces Samsung's Odyssey 3D Hub.
- Per-user installer and uninstaller for setups without RED4ext (one registry key, no admin rights). The installer won't replace
  Samsung's Odyssey 3D Hub unless forced; the uninstaller restores it.
- FOMOD metadata so Vortex installs the files where they belong.
- Two downloads: the Nexus zip for RED4ext setups, and the full zip on GitHub that adds the setup scripts.
- Logging: `braindance3d.log` (levels 0 to 2) and `Braindance3D-setup.log`.
