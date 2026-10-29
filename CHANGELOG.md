# Changelog

## Unreleased

- The game's own DLSS frame generation now works in 3D Mode: 3D Mode no longer turns Frame Generation off or greys
  it out, and the renderer keeps DLSS-G on side-by-side frames. Needs RED4ext; choose DLSS frame generation and
  restart the game once. FSR and XeSS frame generation still turn off in 3D Mode. Done with three small hooks that
  check the game's code first and do nothing if it changed; a real 3D monitor keeps the game's rule.
- The RED4ext plugin's log is now `Braindance3D-hooks.log` (was `Braindance3D-menus.log`).

## 0.2.0 (2026-10-29)

- Menus, the map and loading screens are readable in 3D: the flat picture is squeezed into each half of the
  side-by-side image, so both eyes see the same screen, only while 3D Mode is on (needs RED4ext; `[Menus] Squeeze`
  in `Braindance3D-Settings.ini`). The RED4ext plugin now hooks the game's present to do this.
- Turning 3D Mode off in the game's settings now really turns side-by-side off (before, the mod's engine ini
  forced stereo on).
- The stand-in now reports a real 27" panel size (59.77 x 33.62 cm) instead of 10000. No visible change in testing;
  it's what the game's 3D Mode was tuned on.
- README: recommended setup for Quest 3 and Virtual Desktop (4K virtual monitor, matching refresh rates,
  DLSS Performance with path tracing, frame generation through OptiScaler).

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
