# Changelog

## 0.4.0 (2026-11-01)

- The weapon has real depth in 3D with ray tracing off (tested in game, ray tracing check and Weapon Depth
  counting as 0 under ray tracing included). The game places the first-person camera's two eyes left and right, then
  its weapon-plane rebuild moves both back to the centre, so the arms and the gun were drawn from one point and looked
  flat. A new RED4ext hook puts the eyes back after that rebuild (Weapon Depth sets how far apart). With path tracing
  on, or ray tracing on with any of RT Lighting, Sun Shadows, Local Shadows or Reflections, it steps aside and the
  weapon stays flat: the game's ray-traced light and shadows on it differed between the eyes (darker in one, shadows
  falling the other way). It follows those settings as they change. While ray tracing is on, Weapon Depth also counts
  as 0 (the game's flat weapon is otherwise slid apart between the eyes, which looks wrong); the setting keeps its
  value. The log shows the weapon's eye gap whenever it changes. It reads those settings through lookups the frame
  generation and screen effects checks find, so if those checks fail the weapon stays flat too. Only while SBSDebug is
  on; it checks the game's code first, and if that changed, only the weapon stays flat.
- Switching 3D Mode on in the menu now also turns Depth of Field, Lens Flare, Motion Blur and Vignette off (tested
  in game). It also turns on Remove HUD Visual Effects and Remove HUD Lens Distortion (Accessibility, Interface),
  whose blur and lens bend ghost on the HUD. They all differ between the eyes in side by side. The game
  itself already turns Chromatic Aberration and Film Grain off on that switch; the mod does the rest right after it,
  through the game's own setters. Nothing is locked: turn one back on and it stays on until the game next turns its
  own two off (a switch to 3D in the menu, and possibly other menu events the game treats the same way). Not at game
  start, only while SBSDebug is on, and only if the game's code matches what the mod expects (else just the game's two
  go off). The log lists which were turned off.
- Nexus page and README: weapon depth and the effects switch in the feature lists; without RED4ext the weapon stays
  flat and the effects stay as they are.
- Technical notes: the game's upscaler jitter in 3D, and why the mod leaves it as it is. Each eye's picture moves only
  half as far sideways as DLSS is told (found by the Cyberpunk VR port), but matching the two made foliage and NPCs
  shimmer clearly more in game (DLSS Performance with path tracing and Ray Reconstruction), and the stock picture is
  stable.
- README: which HUD and screen effects ghost in 3D, and the game settings that turn them off.
- README: on an RX 9000, pick FSR4 and restart the game. Changelog 0.3.1: FSR 3.1 was tested on an NVIDIA card only.
- Nexus page: the intro names FSR frame generation too, and "Good to know" is a heading like the other sections.
- Nexus page: says in red that FSR4 and XeSS frame generation are untested (DLSS and FSR 3.1 are tested), lists Field of View among the depth settings, and drops two lines.
- README and Nexus page: the Field of View slider is in the Graphics settings, not Gameplay.
- Technical notes: the RED4ext plugin keeps DLSS, FSR or XeSS frame generation in 3D Mode, not only DLSS.

## 0.3.1 (2026-10-31)

- The game's FSR 3.1 frame generation now works in 3D Mode too (any GPU, tested on an NVIDIA card only; pick FSR3,
  then restart the game: the game fixes its frame generation at start, as with DLSS). FSR 4 frame generation (RX 9000)
  is allowed too, through the same code, but untested. Same hooks as DLSS: the FSR part checks the game's code first,
  and if it changed, only FSR keeps the game's rule.
- The game's XeSS frame generation (Intel Arc) is allowed in 3D Mode too, untested (pick XESS, then restart the
  game). Same hooks again: the XeSS part checks the game's code first, and if it changed, only XeSS keeps the game's
  rule.
- The SBSDebug setting is read with a volatile read, so the compiler can never reuse an earlier read of it.
- README and Nexus page: lower the game's Field of View (e.g. 90 to 80) for life-sized, deeper 3D.
- Technical notes and source comments now say what the reported panel size really feeds (one parallax limit), that
  the reported eye positions don't set the eye separation, and how Depth maps to it.
- Technical notes: the "Ideas for later" section is gone: FSR and XeSS frame generation in 3D Mode are done, and full
  SBS and head tracking are out of scope.

## 0.3.0 (2026-10-30)

- The game's own DLSS frame generation now works in 3D Mode: 3D Mode no longer turns Frame Generation off or greys
  it out, and the renderer keeps DLSS-G on side-by-side frames. Needs RED4ext; choose DLSS frame generation and
  restart the game once (RTX 40 or 50). FSR and XeSS frame generation still turn off in 3D Mode, as does Frame
  Generation on PCs without DLSS frame generation. Done with four small hooks that
  check the game's code first and do nothing if it changed; a real 3D monitor keeps the game's rule.
- The menu squeeze works with the game's DLSS frame generation on (Streamline then wraps the game's D3D12 queue; the
  squeeze uses the native one).
- The menu squeeze now follows the game's own 3D Mode switch. Before, it guessed from display re-checks, and a
  resolution change could flip it, so menus were squeezed in 2D and not in 3D.
- The RED4ext plugin's log is now `Braindance3D-hooks.log` (was `Braindance3D-menus.log`).
- The auto-setup log in `%LOCALAPPDATA%\Braindance3D` (used when the game folder is read-only) now starts over past
  256 KB, like the one in the game folder.
- With OptiScaler's `FGOutput = DLSSG`, the game drops DLSS from its Frame Generation list. Setting `FGInput` and
  `FGOutput` to `nofg` brings it back (README).
- Known limitation: overlays drawn over the game look split in 3D (fine in 2D): Cyber Engine Tweaks' overlay during
  gameplay (fine over a game menu), and OptiScaler's menu and FPS counter always.

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
