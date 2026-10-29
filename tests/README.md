# Tests

Two suites, plus a check of the release zip. CI runs all of them (`.github/workflows/tests.yml`, job `tests` on
windows-latest, job `package` on ubuntu-latest).

| What | Where | Entry point |
|---|---|---|
| The DLL (`src/braindance3d_sdk.c`), black box | `tests/native/test_dll.c` | `tests/run-native-tests.sh` |
| The DLL, white box (`#include`s the .c) | `tests/native/test_whitebox.c` | `tests/run-native-tests.sh` |
| The RED4ext auto-setup plugin (`src/braindance3d_autosetup.c`), white and black box | `tests/native/test_autosetup.c` | `tests/run-native-tests.sh` |
| The menu squeeze's detection, UserSettings.json seed and settings (`src/braindance3d_menus.c`), white box | `tests/native/test_menus.c` | `tests/run-native-tests.sh` |
| The installer (`Braindance3D-Setup.ps1`) | `tests/powershell/*.Tests.ps1` (Pester 5) | `tests/Run-PesterTests.ps1` |
| The release zip | `tests/check-dist.sh` | `./build.sh dist && tests/check-dist.sh [--repro]` |

## C tests

```sh
tests/run-native-tests.sh [--skip-build] [--known-bugs] [filter ...]
```

The script builds both DLLs with `./build.sh` (unless you pass `--skip-build`). It then builds `build/tests/test_dll.exe`,
`build/tests/test_whitebox.exe`, `build/tests/test_autosetup.exe` and `build/tests/test_menus.exe` and runs them. A filter is a substring of a test name. The exit code is 0 when
everything passed, 1 when a test failed, and 2 when a test crashed or hung. You can also run an exe directly:
`test_dll.exe --dll path\to\Odyssey3DSDK.dll [--list] [--verbose] [--known-bugs] [--timeout ms] [filter]`
(`test_autosetup.exe` also takes `--plugin path\to\Braindance3D.dll`).

- **Windows** (Git Bash with MinGW-w64 `gcc`/`windres` on PATH): the tests run natively.
- **Linux**: the script uses `x86_64-w64-mingw32-gcc` and `wine`. Tests that create windows need a display, so
  without `DISPLAY` they run under `xvfb-run`. Non-ASCII paths need a UTF-8 locale, so the script sets
  `LC_ALL=C.UTF-8` when needed. A few tests skip under Wine: the read-only-folder ACL test, and the multi-monitor
  tests unless there are two monitors.

How the harness works:

- Each test copies the DLL to `<temp>\GameLauncherData\core\` and loads it from there, as the game does. It then
  reads the log and settings in that temp install folder.
- The harness sets `BRAINDANCE3D_ANY_EXE=1` because the test exe is not `Cyberpunk2077.exe`. The host-check tests
  clear that variable, and they copy the test exe to `Cyberpunk2077.exe` and run it as a child process.
- `LOCALAPPDATA` points to a temp folder, so the tests never write to the real one.
- The plugin tests call `auto_register()` directly with a scratch key. To test `Main()` the way RED4ext calls it,
  they copy the test exe to `<temp>\<game>\bin\x64\Cyberpunk2077.exe` and put the plugin in
  `red4ext\plugins\Braindance3D`. That child process points `HKEY_CURRENT_USER` at a scratch key with
  `RegOverridePredefKey`. It checks that the redirect works before it calls `Main`. Every `Main` test also checks
  that the real `HKCU\SOFTWARE\Samsung` is unchanged afterwards.
- Wine resets `%ProgramFiles%` in new processes, so under Wine the test that runs `Main` against Samsung's default
  folder is skipped. The white-box tests still cover that rule.

The framework is `tests/native/tinytest.h` (self-contained, no dependencies). `TEST(name)` declares a test, and
`CHECK*`/`REQUIRE*`/`SKIP` work as in most frameworks. `tt_defer` registers a cleanup.

## PowerShell tests

```powershell
pwsh tests/Run-PesterTests.ps1 [-IncludeKnownBugs] [-InstallPester] [-OutputFile results.xml]
powershell -ExecutionPolicy Bypass -File tests\Run-PesterTests.ps1 -CI   # Windows PowerShell 5.1
```

You need Pester 5.3 or newer. `-InstallPester` and `-CI` install it for the current user.

- The tests dot-source the setup script and call its functions. They also run it as a process
  (`-File ... -NoPause`) to check exit codes.
- **Registry safety** (the C tests follow the same rule): the tests only use throwaway keys at `HKCU:\SOFTWARE\Braindance3D-Tests\<guid>`. Those keys
  are removed after each test. `Assert-TestKey` refuses any other path, so the real `HKCU\SOFTWARE\Samsung` is never
  touched.
- On non-Windows systems the registry tests are skipped. For development you can set `BD3D_FAKE_REGISTRY=1` to run
  them against an in-memory HKCU (`tests/powershell/FakeRegistry.ps1`). Only CI on Windows exercises the real
  registry.
- Keep the test files ASCII-only, because Windows PowerShell 5.1 reads BOM-less files as ANSI.

## Known bugs

A test that shows an unfixed bug is kept in the suite but excluded by default, so CI stays green:

- In C, use `KNOWN_BUG_TEST(name, "why")`. Run these tests with `--known-bugs` or `BD3D_RUN_KNOWN_BUGS=1`.
- In Pester, use `-Tag KnownBug`. Run these tests with `-IncludeKnownBugs` or `BD3D_RUN_KNOWN_BUGS=1`.

When a bug is fixed, turn its test into a plain `TEST` (or remove the tag).
