# Changelog

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
