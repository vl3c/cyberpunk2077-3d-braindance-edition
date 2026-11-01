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
`AimIpdCooldownMS` (150, the fade time of both eye separations). The user settings live in `/video/display`:
`Odyssey3D_StereoEnabled`, `Odyssey3D_Preset`, `Odyssey3D_Strength` (Depth), `Odyssey3D_WeaponStrength`,
`Odyssey3D_MotionComfort`.

**Depth.** The game turns Depth into a strength `s` with a hard ceiling: `s = Depth / 0.7` up to Depth 0.7 (so
`s` is 1.0 there), then it climbs to 2.0 at Depth 1.0, and values past 1.0 do nothing. The eye separation (in
metres) starts at `near × s / 19.75`, is capped at `min(0.1 × s, 0.3)` and is then lowered by comfort limits; the
eye positions the SDK reports don't enter it. For a stronger 3D effect than Depth 1.0, lower the game's **Field of
View**: in Virtual Desktop, 90 to 80 made the world look life-sized and the depth much stronger.

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
| device | 1 (+0x08) `GetEyePosition(float3* out, int eye)` | ±32 mm, 0, 600 mm. Called for both eyes every stereo frame and copied into the eye cameras, but the eye separation isn't computed from it (it comes from Depth) |
| device | 2 (+0x10) `GetResolution(float2* out)` | size of the 3D monitor (cached up to 0.5 s) |
| device | 3 (+0x18) `GetFloat2_18(float2* out)` | 59.77 × 33.62, a real 27" panel in cm. The game keeps x × 0.01 once, at device set-up, as the panel width for one parallax limit (its own default is 0.6141 m). That limit can only lower the separation and stops mattering above about 1.18 m, which is why 10000 and 120 × 67.5 looked the same. The per-frame reads are only copied |
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
| `red4ext\plugins\Braindance3D\Braindance3D.dll` | RED4ext plugin that writes the registry setting at game start, squeezes menus and keeps the game's frame generation (DLSS, FSR or XeSS) in 3D Mode |
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
  the game's direct queue with its own command list and fence. With DLSS-G loaded, the game's queue is a Streamline
  proxy (vtable in `sl.interposer.dll`) while the back buffers belong to the native device, so the plugin asks
  `slGetNativeInterface` for the native queue behind it. Shaders are compiled when the plugin loads, with Windows'
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
- **Overlays:** CET draws inside the game's own present, before the squeeze, so its window is squeezed with menus
  (but split over 3D gameplay, which isn't squeezed).
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
  FSR FG sits behind the same view checks, right after the DLSS-G check: `IsFsrFgEnabled` (`RVA 0x2288250`, a read
  of two flags on the renderer's FSR object at `+0x4668`) decides feature 89. XeSS FG too: a read of `+0xDD` on the
  renderer's XeSS object at `+0x4678` decides feature 75.

The RED4ext plugin (`src/braindance3d_framegen.c`) attaches four detours, resolved by hash from CDPR's address
library like `GpuApi::Present`:

| Detour | What it changes |
| --- | --- |
| 3D Mode handler | Marks that it's running; afterwards clears Frame Generation's greyed flag when it was kept, and tells the menu squeeze which way 3D Mode was set |
| `SetFrameGeneration` | Skips the handler's Off when Frame Generation is DLSS (and the setting's list offers it: an RTX 40 or 50), FSR3 (FSR 3.1, every GPU), FSR4 (AMD's AI frame generation on RX 9000, untested), XESS (Intel Arc, untested), or Off while one of those can be picked. Before the list is built, the saved value decides alone. Values it doesn't know keep the game's rule (Off, greyed out), as do FSR3 and FSR4 if the FSR checks at start-up failed, XESS if the XeSS checks failed, and Off when nothing in the list runs in 3D (for example no DLSS, because OptiScaler holds DLSS-G, and the FSR checks failed) |
| `FlagCompute`, `IsDlssgEnabled` | FlagCompute asks `IsDlssgEnabled` only for a view that passed its frame generation checks, so that answer is the game's own decision for the view. It's noted per thread (FlagCompute runs for several views at once on worker threads), and on stereo frames feature 88 is set again from it. For a view that was asked, feature 89 is set again when the game's `IsFsrFgEnabled` says yes (called by the plugin, not hooked). It covers FSR3 and FSR4 alike: both run through the same FidelityFX frame generation DLL. Likewise feature 75 is set again when the renderer's XeSS object (`+0x4678`) has its frame generation flag (`+0xDD`) set, the field FlagCompute itself reads there |

They change nothing unless `[Odyssey3D] SBSDebug` is on (read on every call), so a real 3D monitor keeps the
game's rule. Each is entered through a small assembly thunk that keeps `rcx`, `rdx`, `r8`-`r11` and `xmm0`-`xmm5`:
the game is built with whole-program optimisation, and FlagCompute keeps using `rcx` after calling
`IsDlssgEnabled`. The selected Frame Generation value is read from the setting itself (`+0x84` indexes the list of
values at `+0x90`; before the list is built, the value from the file is at `+0x58`), because the game's
`GetFrameGenerationMode` reads only the file value, which is emptied once the list is built.

Before attaching anything, the plugin checks byte patterns in both groups (an attached hook replaces its target's first
bytes with a jump, and the second group reads `SetFrameGeneration`'s). Each group is attached whole or not at all:

- **3D Mode handler:** its start, the Off call going to `SetFrameGeneration`, and that call being its only one. The
  handler has no exit other than its 3D-on branch (which starts with that call) and its 3D-off branch, so the call
  tells the menu squeeze which way 3D Mode was set.
- **Frame generation:** the grey, `SetFrameGeneration`'s start and settings lookup, FlagCompute's start, its DLSS-G
  check calling `IsDlssgEnabled` (its only call there) and the stereo clear, and the SBSDebug var's name. Then the
  FSR part, which only decides FSR3 and FSR4: the `IsFsrFgEnabled` call between the DLSS-G check and the clear (its
  only call there, and its target's bytes), and the clear of 89 after the one of 88. Then the XeSS part: the read of
  the XeSS flag between the DLSS-G check and the clear, and the clear of 75 after the one of 88. If one of those two
  parts fails, the others still work.

If the second group fails, the game keeps its frame generation rule but the squeeze still follows 3D Mode; if the first
fails, nothing is attached. The hooks log says which check failed. DLSS-G loads only when Frame Generation is DLSS at
game start (Streamline is set up once), so choosing it needs a restart, as in 2D. FSR frame generation is the same:
the FSR object turns it on only while the chosen mode equals the one at start. So is XeSS: its object sets its flag
only when XESS was chosen at start. With FSR at start the game presents through FidelityFX's frame generation swap
chain (`amd_fidelityfx_framegeneration_dx12.dll`), with XeSS through Intel's (`libxess_fg.dll`); the menu squeeze
patches the Present of whichever swap chain the game uses (tested with Streamline's and FidelityFX's). The game offers
XESS only when `xefgSwapChainD3D12CreateContext` succeeded at start (in practice an Intel Arc GPU).

## Upscaler jitter in 3D

Each frame the game moves the camera by a fraction of a pixel (the jitter, `+0x370` in the camera descriptor) and
tells DLSS, FSR or XeSS the offset; the upscaler builds a sharper picture from those offsets. The camera build
(`RVA 0x296EA0`) turns the jitter into a screen offset as `2 * jitter / width` (`RVA 0xC28EC0`), with the width at
`+0x378`. The two eye cameras (`+0x399` bit 1, `+0x398` = 1 left, 2 right) get the width of the whole side-by-side
frame, but each eye fills only half of it. So each eye's picture moves half a pixel sideways while the upscaler is told
a whole one (vertical is right). The Cyberpunk VR port found and measured this
([docs/engine-stereo-plan.md](https://github.com/dariulone/cyberpunk-vr-port/blob/main/docs/engine-stereo-plan.md),
"Lamp flicker with DLSS only").

A fifth detour, on the camera build, hands an eye camera half that width for the build and then puts the descriptor's
width back, so anything else that reads it sees the game's value. Widths under 2 (no jitter: the previous frame's
camera) and every other camera are left alone, and so is everything while `SBSDebug` is off. Before attaching, the
plugin checks the build's start, its eye branch, the jitter call going to the jitter offset function (which must also
be where its own hash resolves, double the jitter and divide by the size) and being its only call there. If anything
doesn't match, only this detour stays off; it's its own group.

## Building and testing

`./build.sh` builds the DLL with MinGW-w64 (no PE timestamp, so builds of the same source with the same
toolchain are identical). `./build.sh dist` also writes the release zip and its SHA-256. Tests are in `tests/`.

