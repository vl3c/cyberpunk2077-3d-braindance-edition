#!/usr/bin/env bash
# Builds Odyssey3DSDK.dll with MinGW-w64 and assembles the release zip.
#   ./build.sh          build the DLL into build/ and copy it into package/
#   ./build.sh dist     also write dist/3D-Braindance-Edition-<version>.zip
# The DLL is reproducible for a given toolchain (no PE timestamp). Release builds use
# x86_64-w64-mingw32-gcc 13 (Ubuntu 24.04 package gcc-mingw-w64-x86-64-win32).
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
VERSION=$(sed -n 's/^#define BD3D_VERSION_STR "\(.*\)"/\1/p' src/version.h)
MODDIR=package/bin/x64/Braindance3D
CORE=$MODDIR/GameLauncherData/core

# The version lives in a few places; refuse to build if they disagree.
check_version() {
    local file=$1 pattern=$2
    if ! grep -q -- "$pattern" "$file"; then
        echo "version mismatch: $file does not contain '$pattern' (version.h says $VERSION)" >&2
        exit 1
    fi
}
check_version "$MODDIR/Braindance3D-Setup.ps1" "\$script:Version = '$VERSION'"
check_version package/fomod/info.xml "<Version>$VERSION</Version>"
check_version CHANGELOG.md "## $VERSION"

mkdir -p build "$CORE"
"$WINDRES" -I src src/version.rc -O coff -o build/version.o
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -Werror -std=gnu11 -shared -static-libgcc -s \
    -Wl,--no-insert-timestamp -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va \
    -o build/Odyssey3DSDK.dll src/braindance3d_sdk.c build/version.o -luser32
cp build/Odyssey3DSDK.dll "$CORE/Odyssey3DSDK.dll"
R4E=package/red4ext/plugins/Braindance3D
mkdir -p "$R4E"
"$WINDRES" -I src -DBD3D_AUTOSETUP src/version.rc -O coff -o build/version-autosetup.o
"$CC" -O2 -Wall -Wextra -Wno-unused-parameter -Werror -std=gnu11 -shared -static-libgcc -s \
    -Wl,--no-insert-timestamp -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va \
    -o build/Braindance3D.dll src/braindance3d_autosetup.c build/version-autosetup.o -ladvapi32 -lversion
cp build/Braindance3D.dll "$R4E/Braindance3D.dll"
echo "built build/Odyssey3DSDK.dll ($VERSION) sha256 $(sha256sum build/Odyssey3DSDK.dll | cut -d' ' -f1)"
echo "built build/Braindance3D.dll ($VERSION) sha256 $(sha256sum build/Braindance3D.dll | cut -d' ' -f1)"

if [[ "${1:-}" == "dist" ]]; then
    # Stage an explicit file list, so nothing else in package/ (logs from testing) can ship.
    STAGE=$(mktemp -d)
    trap 'rm -rf "$STAGE"' EXIT
    FILES=(
        bin/x64/Braindance3D/GameLauncherData/core/Odyssey3DSDK.dll
        bin/x64/Braindance3D/Braindance3D-Settings.ini
        bin/x64/Braindance3D/Braindance3D-Setup.ps1
        bin/x64/Braindance3D/Install-Braindance3D.bat
        bin/x64/Braindance3D/Uninstall-Braindance3D.bat
        engine/config/platform/pc/braindance3d.ini
        red4ext/plugins/Braindance3D/Braindance3D.dll
        fomod/info.xml
        fomod/ModuleConfig.xml
    )
    for f in "${FILES[@]}"; do
        mkdir -p "$STAGE/$(dirname "$f")"
        cp "package/$f" "$STAGE/$f"
    done
    cp README.md LICENSE CHANGELOG.md "$STAGE/bin/x64/Braindance3D/"
    # Windows line endings for the text files users open or run.
    find "$STAGE" -type f \( -name '*.ini' -o -name '*.ps1' -o -name '*.bat' -o -name '*.md' -o -name 'LICENSE' -o -name '*.xml' \) \
        -exec sed -i 's/\r$//; s/$/\r/' {} +
    # Fixed timestamps and order, so the zip is reproducible.
    EPOCH=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct 2>/dev/null || echo 1767225600)}
    find "$STAGE" -exec touch -h -d "@$EPOCH" {} +
    mkdir -p dist
    ZIP="$PWD/dist/3D-Braindance-Edition-$VERSION.zip"
    rm -f "$ZIP"
    (cd "$STAGE" && find . -type f | sed 's|^\./||' | LC_ALL=C sort | zip -q -X -@ "$ZIP")
    (cd dist && sha256sum "$(basename "$ZIP")" > "$(basename "$ZIP").sha256")
    echo "wrote $ZIP"
    unzip -l "$ZIP"
fi
