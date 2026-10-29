#!/usr/bin/env bash
# Builds the DLLs (the SDK stand-in and the RED4ext auto-setup plugin) and the native C tests, then runs them.
#
#   tests/run-native-tests.sh [--skip-build] [--known-bugs] [test name filters...]
#
# Linux: cross-compiles with x86_64-w64-mingw32-gcc and runs the tests under Wine (WINEDEBUG=-all wine).
# Windows (Git Bash / MSYS2): compiles with gcc/windres from PATH and runs the tests natively.
# CC and WINDRES override the compilers. Test binaries go to build/tests/. Exit code: 0 when all passed.
set -euo pipefail
cd "$(dirname "$0")/.."

SKIP_BUILD=0
ARGS=()
for a in "$@"; do
    case "$a" in
        --skip-build) SKIP_BUILD=1 ;;
        *) ARGS+=("$a") ;;
    esac
done

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) NATIVE=1 ;;
    *) NATIVE=0 ;;
esac

if [[ $NATIVE == 1 ]]; then
    export CC=${CC:-gcc}
    export WINDRES=${WINDRES:-windres}
    RUN=()
else
    export CC=${CC:-x86_64-w64-mingw32-gcc}
    export WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
    if ! command -v wine > /dev/null; then
        echo "wine is needed to run the Windows test binaries on $(uname -s)" >&2
        exit 2
    fi
    export WINEDEBUG=${WINEDEBUG:--all}
    # Wine converts file names with the Unix locale: a UTF-8 one is needed for the non-ASCII path tests.
    if [[ "$(locale charmap 2> /dev/null)" != "UTF-8" ]]; then
        export LC_ALL=C.UTF-8
    fi
    RUN=(wine)
    # Wine needs an X server to create windows (the monitor tests create a "game window").
    if [[ -z "${DISPLAY:-}" ]]; then
        if command -v xvfb-run > /dev/null; then
            RUN=(xvfb-run -a -s "-screen 0 1024x768x24" wine)
        else
            echo "note: no DISPLAY and no xvfb-run: the tests that need a window will be skipped" >&2
        fi
    fi
fi

if [[ $SKIP_BUILD == 0 ]]; then
    ./build.sh
fi
for f in build/Odyssey3DSDK.dll build/Braindance3D.dll; do
    [[ -f $f ]] || { echo "$f is missing: run ./build.sh" >&2; exit 2; }
done

mkdir -p build/tests
CFLAGS=(-O1 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers
    -Werror -static -Itests/native)
LIBS=(-luser32 -ladvapi32 -lversion -lole32)

# The installer recognises its DLL by the ProductName in the version resource; the test checks the built DLL has it.
PRODUCT=$(sed -n 's/.*VALUE "ProductName", "\(.*\)".*/\1/p' src/version.rc)
CFLAGS+=("-DBD3D_PRODUCT_NAME=\"$PRODUCT\"")

echo "== compiling tests with $CC"
"$CC" "${CFLAGS[@]}" -o build/tests/test_dll.exe tests/native/test_dll.c "${LIBS[@]}"
"$CC" "${CFLAGS[@]}" -Isrc -o build/tests/test_whitebox.exe tests/native/test_whitebox.c "${LIBS[@]}"
"$CC" "${CFLAGS[@]}" -Isrc -o build/tests/test_autosetup.exe tests/native/test_autosetup.c src/braindance3d_menus.c \
    "${LIBS[@]}" -ldxguid -luuid
"$CC" "${CFLAGS[@]}" -Isrc -o build/tests/test_menus.exe tests/native/test_menus.c "${LIBS[@]}" -ldxguid -luuid

DLL=build/Odyssey3DSDK.dll
PLUGIN=build/Braindance3D.dll
if [[ $NATIVE == 1 ]] && command -v cygpath > /dev/null; then
    DLL=$(cygpath -w "$DLL")
    PLUGIN=$(cygpath -w "$PLUGIN")
else
    # A relative path: Wine resolves it against the current folder. (winepath would start a wineserver
    # outside Xvfb, and later Wine processes would share its display-less desktop.)
    DLL='build\Odyssey3DSDK.dll'
    PLUGIN='build\Braindance3D.dll'
fi
if [[ $NATIVE == 0 && "${RUN[0]}" == xvfb-run ]] && command -v wineserver > /dev/null; then
    # A wineserver started without a display keeps a desktop that can't create windows: let it exit first.
    wineserver -w 2> /dev/null || true
fi

STATUS=0
for t in test_dll test_whitebox test_autosetup test_menus; do
    echo
    echo "== $t"
    if ! ${RUN[@]+"${RUN[@]}"} "build/tests/$t.exe" --dll "$DLL" --plugin "$PLUGIN" "${ARGS[@]+"${ARGS[@]}"}"; then
        STATUS=1
    fi
done
echo
if [[ $STATUS == 0 ]]; then echo "== native tests: all passed"; else echo "== native tests: FAILURES (see above)"; fi
exit $STATUS
