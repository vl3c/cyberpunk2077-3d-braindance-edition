# How it works

## The game side

Cyberpunk 2077 2.5 (`Cyberpunk2077.exe` 3.0.88.46235, product version 2.5) has a full stereo renderer for
Samsung's Odyssey 3D monitor: right-eye render targets, stereo ray tracing and stereo UI depth. At startup it:

1. Reads `HKCU\SOFTWARE\Samsung\Odyssey3DHub`, value `InstallPath` (fallback `C:\Program Files\Odyssey 3D Hub`).
2. Loads `<InstallPath>\GameLauncherData\core\Odyssey3DSDK.dll` and calls `create_odyssey3d_factory_instance`.
3. Asks the factory for a device and checks that the device reports a display that is available and matches
   the monitor the game window is on. If any step fails, the 3D Mode setting stays greyed out.

The hidden engine config group `Odyssey3D` (set in `engine\config\platform\pc\braindance3d.ini`) controls the output:

| Option | Default | This mod | Meaning |
| --- | --- | --- | --- |
| `OverrideEnabled` | false | false | When true, the engine takes stereo on/off from `StereoEnabled` below instead of the user setting |
| `StereoEnabled` | true | true | Stereo rendering, used only when `OverrideEnabled` is true |
| `SBSDebug` | false | true | Present both eyes side by side instead of handing them to the SDK's weaver |

With `OverrideEnabled` off, the 3D Mode toggle in the game's settings decides whether stereo is on: turning it
off gives the normal 2D picture, live, and `SBSDebug` still applies when it is on. With the override on (0.1.0),
stereo stayed on whatever the toggle said.

Other options in the group (not changed): `WeaponConvergenceDistance` (0.5), `AO/StereoMod` (true),
`AO/NoDownsampleThreshold` (1440), `Dynamic3DSettings/EnableWidgetDepth` (true), `WidgetDepthValue` (0.008),
`AimIpdCooldownMS` (150). The user settings live in `/video/display`: `Odyssey3D_StereoEnabled`,
`Odyssey3D_Preset`, `Odyssey3D_Strength` (Depth), `Odyssey3D_WeaponStrength`, `Odyssey3D_MotionComfort`.

## The SDK interface

Worked out from the game's call sites; only what the game uses is implemented (`src/braindance3d_sdk.c`). All
objects are C++-style: a pointer to a vtable, Microsoft x64 calling convention.

Exports: `create_odyssey3d_factory_instance()` (returns the factory, or NULL in any process other than
`Cyberpunk2077.exe`, because the registry key is shared by every game with Odyssey 3D support),
`destroy_odyssey3d_factory_instance(factory)`, and `braindance3d_version()` (a version string, for diagnostics).

| Object | Slot (vtable offset) | Implemented as |
| --- | --- | --- |
| factory | 0 (+0x00) `CreateDevice(Device** out)` | heap device, returns `out` |
| factory | 2 (+0x10) `CreateWeaver(shared_ptr* ret, device, ID3D12Device*, allocator, ?, 0, 0, HWND)` | logging weaver in an MSVC `std::shared_ptr` |
| device | 1 (+0x08) `GetEyePosition(float3* out, int eye)` | ±32 mm, 0, 600 mm |
| device | 2 (+0x10) `GetResolution(float2* out)` | size of the 3D monitor (cached up to 0.5 s) |
| device | 3 (+0x18) `GetFloat2_18(float2* out)` | 59.77 × 33.62, a real 27" panel in cm (the game stores x × 0.01). Read every stereo frame; 10000 and 120 × 67.5 looked the same |
| device | 6 (+0x30) `GetMonitorRect(int64[4] out)` | `{top, left, right, bottom}` of the 3D monitor (always current) |
| device | 9 (+0x48) `Hint48(bool)` | no-op returning 0; called 400 to 900 times a second, result ignored |
| device | 10 (+0x50) `IsAvailable()` | true |
| device | 12 (+0x60) scalar deleting destructor | frees the device |
| weaver | 2 (+0x10) `Weave(w, h, x, y)`, 4 (+0x20) `SetInputViewTexture`, 6 (+0x30) `SetOutputFrameBuffer`, 7 (+0x38) `SetCommandList` | logging no-ops |

The "3D monitor" is the monitor of the game's main window (the foreground window if it belongs to the game,
else the largest visible top-level window of the process, skipping tool, overlay and console windows), or the
primary monitor before that window exists or when `Monitor = primary` is set. None of this sends window
messages, so a busy window thread can't block the game's render threads.

Every other slot of the 32-entry vtables logs its arguments and returns its first argument. In two 5-minute
test sessions the game called only the slots above, about 110 times a second for `IsAvailable` and
`GetEyePosition`. With `SBSDebug` on, it never creates a weaver.

## Files

| Path in the game folder | What |
| --- | --- |
| `engine\config\platform\pc\braindance3d.ini` | The engine options above |
| `bin\x64\Braindance3D\GameLauncherData\core\Odyssey3DSDK.dll` | The SDK stand-in |
| `bin\x64\Braindance3D\Braindance3D-Settings.ini` | Log level, monitor choice and the menu squeeze |
| `bin\x64\Braindance3D\Braindance3D-Setup.ps1` | Install, uninstall and status (registry only) |
| `bin\x64\Braindance3D\Install-Braindance3D.bat`, `Uninstall-Braindance3D.bat` | Double-click wrappers |
| `bin\x64\Braindance3D\README.md`, `LICENSE`, `CHANGELOG.md` | Docs |
| `bin\x64\Braindance3D\braindance3d.log`, `braindance3d.old.log`, `Braindance3D-setup.log` | Logs, written at run time (or in `%LOCALAPPDATA%\Braindance3D`) |
| `red4ext\plugins\Braindance3D\Braindance3D.dll` | RED4ext plugin that writes the registry setting at game start, squeezes menus and keeps DLSS frame generation in 3D Mode |
| `bin\x64\Braindance3D\Braindance3D-hooks.log`, `Braindance3D-hooks.old.log` | What the menu squeeze and the frame generation hooks did (this run and the previous one) |
| `bin\x64\Braindance3D\Braindance3D-autosetup.log` | What the RED4ext plugin did (appended each start, reset past 256 KB) |
| `fomod\` | Install metadata for Vortex and other FOMOD-aware managers |

## Registration

The game reads the SDK location from the registry, which mod managers can't write. Two things can write it, with
the same rules: never replace another working SDK such as Samsung's Odyssey 3D Hub, and remember what was there.
An `InstallPath` counts as unused only when its folder is missing on a local drive that is present, and Samsung's
default folder has no SDK either. A network path, a missing drive (an offline USB disk), a quoted path or a
value that isn't text is left alone (the script replaces those only with `-Force`, never a non-text value):

- `red4ext\plugins\Braindance3D\Braindance3D.dll` (`src/braindance3d_autosetup.c`): a RED4ext plugin. RED4ext
  loads plugins about 10 seconds before the game looks for the SDK (measured on 2.5 with RED4ext 1.31), so the
  first launch already works. It points `InstallPath` at `<folder of Cyberpunk2077.exe>\Braindance3D`. It also
  installs the menu squeeze (see below).
- `Braindance3D-Setup.ps1` (via the .bat files), for setups without RED4ext. It can also uninstall, and with
  `-Force` replace Samsung's Hub.

Both write these values in `HKCU\SOFTWARE\Samsung\Odyssey3DHub`:

| Value | Meaning |
| --- | --- |
| `InstallPath` | The folder the game loads the SDK from (`<game>\bin\x64\Braindance3D`) |
| `Braindance3D_InstallPath` | The path this mod wrote, to recognise it later |
| `Braindance3D_PreviousInstallPath` | What uninstall restores, or `<none>` |
| `Braindance3D_InstalledBy` | The mod version that installed it |

## Menu squeeze

The game draws menus, the map and loading screens once across the whole window. In half-SBS each eye would see a
different half, so the RED4ext plugin (`src/braindance3d_menus.c`) squeezes those frames into each half.

- **Hook:** RED4ext resolves `GpuApi::Present` and the GPU device data by hash (the addresses Cyber Engine Tweaks
  uses). A pass-through hook on `GpuApi::Present` finds the game's swap chain on its first call and patches its
  `Present` and `Present1` vtable slots (on 2.5 with DLSS-G that vtable is Streamline's `sl.interposer.dll`).
- **Squeeze:** at that point the back buffer is finished and in the PRESENT state. The plugin copies it, draws
  one full-screen triangle that samples `x * 2` for the left half and `x * 2 - 1` for the right, and runs both on
  the game's direct queue with its own command list and fence. Shaders are compiled when the plugin loads, with Windows'
  `d3dcompiler_47.dll`. Any failure turns the squeeze off for the session and is logged.
- **Detection,** once per game frame, from the stand-in's call counters (`braindance3d_device_calls(slot)`):

  | What the game asked since the last frame | Meaning |
  | --- | --- |
  | `GetEyePosition` (both eyes) | Stereo frame: 3D Mode is on, nothing to squeeze |
  | No eye positions | Flat frame (menus, map, loading): squeezed while 3D Mode is on |
  | `IsAvailable` once and no eye positions, 30 frames in a row | 2D gameplay: 3D Mode is off |
  | The game's 3D Mode handler ran (hooked for frame generation, below) | 3D Mode is on if it took its 3D-on branch, else off: at start-up, on a switch in Settings and on a display change |

  At start-up the saved `Odyssey3D_StereoEnabled` in `%LOCALAPPDATA%\CD Projekt Red\Cyberpunk 2077\UserSettings.json`
  is the first guess.
- **Safety:** only the game's own swap chain is squeezed (the patched vtable can be shared by other swap chains),
  at most once per game frame and never re-entered. GPU resources are only replaced once the GPU is done with
  them, and both DLLs are pinned so the patched pointers never dangle.
- **Overlays:** CET draws inside the game's own present, before the squeeze, so its window is squeezed with menus.
  OptiScaler draws its menu and FPS counter in its own swap chain below Streamline, after the squeeze, so those
  still cover both halves.

## Frame generation in 3D Mode

The game keeps its own frame generation off in 3D Mode in two places, both needed for Samsung's weaver (a
generated frame would blend two interlaced pictures) but not for side by side:

- **Settings:** the 3D Mode handler (`RVA 0x2073C00`) calls `SetFrameGeneration(0, 1)` (Off) and sets the greyed
  flag (`0x800` at `+0x44`) on Frame Generation whenever 3D Mode is on and available. It runs at start-up, on
  display changes and when 3D Mode is switched, so a saved "on" never survives.
- **Render:** `FlagCompute` (`RVA 0x227E5D0`) builds each view's render-feature bits every frame. It sets feature
  88 (DLSS-G) from the game's DLSS-G checks, then clears it while stereo is on (likewise 89, FSR FG, and 75, XeSS
  FG). The frame graph and the Streamline resource tagging read feature 88, so without it DLSS-G gets no input.

The RED4ext plugin (`src/braindance3d_framegen.c`) attaches four detours, resolved by hash from CDPR's address
library like `GpuApi::Present`:

| Detour | What it changes |
| --- | --- |
| 3D Mode handler | Marks that it's running; afterwards clears Frame Generation's greyed flag when it was kept, and tells the menu squeeze which way 3D Mode was set |
| `SetFrameGeneration` | Skips the handler's Off unless FSR or XeSS frame generation is selected (those stay blocked in stereo, so the game still turns them off). Off stays choosable, so DLSS can be picked in 3D Mode |
| `FlagCompute`, `IsDlssgEnabled` | FlagCompute asks `IsDlssgEnabled` only for a view that passed its other DLSS-G checks, so that answer is the game's own decision for the view. It's noted per thread (FlagCompute runs for several views at once on worker threads), and on stereo frames feature 88 is set again from it |

They change nothing unless `[Odyssey3D] SBSDebug` is on (read on every call), so a real 3D monitor keeps the
game's rule. Each is entered through a small assembly thunk that keeps `rcx`, `rdx`, `r8`-`r11` and `xmm0`-`xmm5`:
the game is built with whole-program optimisation, and FlagCompute keeps using `rcx` after calling
`IsDlssgEnabled`. The selected Frame Generation value is read from the setting itself (`+0x84` indexes the list of
values at `+0x90`; before the list is built, the value from the file is at `+0x58`), because the game's
`GetFrameGenerationMode` reads only the file value, which is emptied once the list is built.

Before attaching, the plugin checks byte patterns, in two groups that are each attached whole or not at all:

- **3D Mode handler:** its start, the Off call going to `SetFrameGeneration`, and that call being its only one. The
  handler has no exit other than its 3D-on branch (which starts with that call) and its 3D-off branch, so the call
  tells the menu squeeze which way 3D Mode was set.
- **Frame generation:** the grey, `SetFrameGeneration`'s start and settings lookup, FlagCompute's start, its DLSS-G
  check calling `IsDlssgEnabled` (its only call there) and the stereo clear, and the SBSDebug var's name.

If the second group fails, the game keeps its frame generation rule but the squeeze still follows 3D Mode; if the first
fails, nothing is attached. The hooks log says which check failed. DLSS-G loads only when Frame Generation is DLSS at
game start (Streamline is set up once), so choosing it needs a restart, as in 2D.

## Building and testing

`./build.sh` builds the DLL with MinGW-w64 (no PE timestamp, so builds of the same source with the same
toolchain are identical). `./build.sh dist` also writes the release zip and its SHA-256. Tests are in `tests/`.

## Ideas for later

- Full SBS (full resolution per eye) through a real weaver that copies the eyes into a wider back buffer.
- Head tracking through OpenXR, using the eye-position callback.
