#!/usr/bin/env bash
# Checks the release zip that ./build.sh dist writes: layout, line endings, checksum, the DLL inside,
# no stray files, and (with --repro) that building it again gives the same bytes.
#
#   ./build.sh dist && tests/check-dist.sh [--repro]
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^#define BD3D_VERSION_STR "\(.*\)"/\1/p' src/version.h)
FULL=dist/3D-Braindance-Edition-$VERSION.zip
NEXUS=dist/3D-Braindance-Edition-$VERSION-Nexus.zip
for z in "$FULL" "$NEXUS"; do
    if [[ ! -f $z ]]; then
        echo "FAIL: no $z (run ./build.sh dist)" >&2
        exit 1
    fi
done
FAIL=0
fail() { echo "FAIL: $*" >&2; FAIL=1; }
ok() { echo "ok:   $*"; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

check_zip() { # zip, kind (full or nexus)
    local ZIP=$1 KIND=$2
    echo "== $ZIP ($KIND)"
    rm -rf "$WORK/x"
    unzip -q "$ZIP" -d "$WORK/x"

    MOD=bin/x64/Braindance3D
    SCRIPTS=("$MOD/Braindance3D-Setup.ps1" "$MOD/Install-Braindance3D.bat" "$MOD/Uninstall-Braindance3D.bat")
    EXPECTED=(
        "$MOD/GameLauncherData/core/Odyssey3DSDK.dll"
        "$MOD/Braindance3D-Settings.ini"
        "$MOD/README.md"
        "$MOD/LICENSE"
        "$MOD/CHANGELOG.md"
        engine/config/platform/pc/braindance3d.ini
        red4ext/plugins/Braindance3D/Braindance3D.dll
        fomod/info.xml
        fomod/ModuleConfig.xml
    )
    # The full zip (GitHub) has the setup scripts for installs without RED4ext; the Nexus zip leaves them out.
    expected=("${EXPECTED[@]}")
    if [[ $KIND == full ]]; then expected+=("${SCRIPTS[@]}"); fi
    for f in "${expected[@]}"; do
        if [[ -f "$WORK/x/$f" ]]; then ok "contains $f"; else fail "missing $f"; fi
    done
    if [[ $KIND == nexus ]]; then
        for f in "${SCRIPTS[@]}"; do
            if [[ -e "$WORK/x/$f" ]]; then fail "the Nexus zip contains $f"; else ok "leaves out $f"; fi
        done
    fi
    n=$(unzip -Z1 "$ZIP" | grep -vc '/$' || true)
    if [[ $n == "${#expected[@]}" ]]; then ok "$n files, as expected"; else fail "$n files, expected ${#expected[@]}"; fi

    # Nothing that is produced at run time or by tests.
    while IFS= read -r f; do
        fail "unexpected file in the zip: $f"
    done < <(cd "$WORK/x" && find . -type f \( -iname '*.log' -o -iname '*.o' -o -iname '*.exe' -o -iname '*.tmp' \) | sed 's|^\./||')

    # The installer looks for the DLL at <InstallPath>\GameLauncherData\core, the game for the engine ini
    # under engine\, RED4ext for plugins under red4ext\plugins: all relative to the game folder the zip is
    # extracted into.
    TOP='^(bin/|engine/|red4ext/|fomod/)'
    if unzip -Z1 "$ZIP" | grep -qvE "$TOP"; then
        fail "zip has top-level entries other than bin/, engine/, red4ext/ and fomod/: $(unzip -Z1 "$ZIP" | grep -vE "$TOP" | head -3)"
    else
        ok "only bin/, engine/, red4ext/ and fomod/ at the top level"
    fi

    # A mod manager installs through the FOMOD: it must copy every top-level game folder in the zip.
    if [[ -f "$WORK/x/fomod/ModuleConfig.xml" ]]; then
        for d in bin engine red4ext; do
            if grep -q "<folder source=\"$d\" destination=\"$d\"" "$WORK/x/fomod/ModuleConfig.xml"; then
                ok "fomod/ModuleConfig.xml installs $d/"
            else
                fail "fomod/ModuleConfig.xml does not install $d/"
            fi
        done
    fi

    # Text files users open or run have Windows line endings (Notepad, cmd.exe).
    for f in "$MOD/Braindance3D-Settings.ini" "$MOD/Braindance3D-Setup.ps1" "$MOD/Install-Braindance3D.bat" \
        "$MOD/Uninstall-Braindance3D.bat" "$MOD/README.md" "$MOD/LICENSE" "$MOD/CHANGELOG.md" \
        engine/config/platform/pc/braindance3d.ini fomod/info.xml fomod/ModuleConfig.xml; do
        [[ -f "$WORK/x/$f" ]] || continue
        if ! awk '!/\r$/ { bad = 1 } END { exit bad }' "$WORK/x/$f"; then
            fail "$f has lines without CRLF"
        else
            ok "$f uses CRLF"
        fi
    done

    # The setup script must stay ASCII (Windows PowerShell 5.1 reads BOM-less scripts as ANSI).
    if [[ -f "$WORK/x/$MOD/Braindance3D-Setup.ps1" ]]; then
        if LC_ALL=C grep -q $'[\x80-\xff]' "$WORK/x/$MOD/Braindance3D-Setup.ps1"; then
            fail "Braindance3D-Setup.ps1 has non-ASCII bytes"
        else
            ok "Braindance3D-Setup.ps1 is ASCII"
        fi
    fi

    # The DLLs in the zip are the ones just built.
    for pair in "build/Odyssey3DSDK.dll:$MOD/GameLauncherData/core/Odyssey3DSDK.dll" \
        "build/Braindance3D.dll:red4ext/plugins/Braindance3D/Braindance3D.dll"; do
        built=${pair%%:*} zipped=${pair#*:}
        [[ -f "$WORK/x/$zipped" ]] || continue
        if [[ -f $built ]]; then
            if cmp -s "$built" "$WORK/x/$zipped"; then ok "$zipped is $built"; else fail "$zipped differs from $built"; fi
        fi
        if [[ "$(head -c2 "$WORK/x/$zipped")" != "MZ" ]]; then fail "$zipped is not a PE file"; fi
    done

    # Checksum file.
    if [[ -f "$ZIP.sha256" ]]; then
        if (cd "$(dirname "$ZIP")" && sha256sum -c --status "$(basename "$ZIP").sha256"); then ok "sha256 matches"; else fail "sha256 does not match"; fi
    else
        fail "missing $ZIP.sha256"
    fi

    # fomod metadata names this version.
    if [[ -f "$WORK/x/fomod/info.xml" ]]; then
        if grep -q "<Version>$VERSION</Version>" "$WORK/x/fomod/info.xml"; then ok "fomod/info.xml version $VERSION"; else fail "fomod/info.xml does not say $VERSION"; fi
    fi

}

check_zip "$FULL" full
check_zip "$NEXUS" nexus

if [[ "${1:-}" == "--repro" ]]; then
    before=$(sha256sum "$FULL" "$NEXUS" | cut -d' ' -f1)
    ./build.sh dist > /dev/null
    after=$(sha256sum "$FULL" "$NEXUS" | cut -d' ' -f1)
    if [[ "$before" == "$after" ]]; then ok "rebuilding gives the same zips ($(echo $before))"; else fail "rebuilding changed the zips: $(echo $before) -> $(echo $after)"; fi
fi

if [[ $FAIL == 0 ]]; then echo "dist check passed: $FULL $NEXUS"; else echo "dist check FAILED" >&2; fi
exit $FAIL
