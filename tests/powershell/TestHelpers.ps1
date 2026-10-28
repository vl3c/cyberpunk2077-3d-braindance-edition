# Shared helpers for the Pester tests of Braindance3D-Setup.ps1. Dot-source this in BeforeAll.
# Works in Windows PowerShell 5.1 and PowerShell 7 (Windows, Linux, macOS). Keep this file ASCII.
#
# Safety: registry tests only ever use keys under HKCU:\SOFTWARE\Braindance3D-Tests\<guid>, made by
# New-TestKey and removed by Remove-TestKey. Nothing here reads or writes HKCU\SOFTWARE\Samsung.

$script:IsWin = [System.Environment]::OSVersion.Platform -eq 'Win32NT'
$script:RepoRoot = (Resolve-Path -LiteralPath (Join-Path (Join-Path $PSScriptRoot '..') '..')).Path
$script:ModSource = Join-Path $script:RepoRoot 'package\bin\x64\Braindance3D'
$script:SetupScript = Join-Path $script:ModSource 'Braindance3D-Setup.ps1'
$script:TestKeyRoot = 'HKCU:\SOFTWARE\Braindance3D-Tests'

# BD3D_FAKE_REGISTRY=1 on Linux/macOS: run the registry tests against an in-memory HKCU (FakeRegistry.ps1).
$script:FakeRegistry = (-not $script:IsWin) -and ($env:BD3D_FAKE_REGISTRY -eq '1')
$script:HasRegistry = $script:IsWin -or $script:FakeRegistry
if ($script:FakeRegistry) { . (Join-Path $PSScriptRoot 'FakeRegistry.ps1') }

# A string that needs Unicode paths (a-umlaut, e-acute, katakana "te su to"), built from code points so this file stays ASCII.
$script:NonAscii = 'Br' + [char]0x00E4 + 'indance ' + [char]0x00E9 + ' ' + [char]0x30C6 + [char]0x30B9 + [char]0x30C8

# The built DLL, if there is one (./build.sh). Tests that need its version resource skip without it.
$script:BuiltDll = $null
foreach ($candidate in @((Join-Path $script:RepoRoot 'build\Odyssey3DSDK.dll'),
        (Join-Path $script:ModSource 'GameLauncherData\core\Odyssey3DSDK.dll'))) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { $script:BuiltDll = $candidate; break }
}

function Get-VersionH {
    $text = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'src\version.h') -Raw
    return [regex]::Match($text, '#define BD3D_VERSION_STR "([^"]+)"').Groups[1].Value
}

function Get-RcProductName {
    $text = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'src\version.rc') -Raw
    return [regex]::Match($text, 'VALUE "ProductName", "([^"]+)"').Groups[1].Value
}

# ---------------------------------------------------------------- temp folders

# Makes a fresh test root in the system temp folder and points $env:TEMP somewhere else, because the
# installer refuses to install from inside $env:TEMP. Restore-TestRoot undoes both.
function New-TestRoot {
    $base = [System.IO.Path]::GetTempPath()
    $root = Join-Path $base ('bd3d-pester-' + [guid]::NewGuid().ToString('N').Substring(0, 12))
    [System.IO.Directory]::CreateDirectory($root) | Out-Null
    $fakeTemp = Join-Path $root 'not-the-mod-temp'
    [System.IO.Directory]::CreateDirectory($fakeTemp) | Out-Null
    $script:SavedTemp = $env:TEMP
    $script:SavedLocalAppData = $env:LOCALAPPDATA
    $env:TEMP = $fakeTemp
    $env:LOCALAPPDATA = Join-Path $root 'localappdata'
    return $root
}

function Restore-TestRoot {
    param([string]$Root)
    $env:TEMP = $script:SavedTemp
    $env:LOCALAPPDATA = $script:SavedLocalAppData
    if ($Root -and (Test-Path -LiteralPath $Root)) {
        Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
    }
}

function New-File {
    param([string]$Path, [string]$Text = 'x')
    # .NET calls: no wildcard handling of [ ] in any PowerShell version.
    [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Path)) | Out-Null
    [System.IO.File]::WriteAllText($Path, $Text)
}

# Lays out <Root>\<Name>\bin\x64\Braindance3D like an extracted mod in a game folder and returns that folder.
#   -NoGame: no Cyberpunk2077.exe; -NoDll: no SDK DLL; -NoSettings: no Braindance3D-Settings.ini;
#   -NoEngineIni: no engine ini; -RealDll: copy the built DLL (else a dummy file).
function New-ModFolder {
    param([string]$Root, [string]$Name = 'Cyberpunk 2077', [switch]$NoGame, [switch]$NoDll, [switch]$NoSettings,
        [switch]$NoEngineIni, [switch]$RealDll)
    $game = Join-Path $Root $Name
    $mod = Join-Path $game 'bin\x64\Braindance3D'
    $mod = [System.IO.Directory]::CreateDirectory($mod).FullName
    [System.IO.File]::Copy($script:SetupScript, (Join-Path $mod 'Braindance3D-Setup.ps1'), $true)
    if (-not $NoDll) {
        $dll = Join-Path $mod 'GameLauncherData\core\Odyssey3DSDK.dll'
        if ($RealDll) {
            [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($dll)) | Out-Null
            [System.IO.File]::Copy($script:BuiltDll, $dll, $true)
        } else {
            New-File $dll 'MZ not a real DLL'
        }
    }
    if (-not $NoSettings) { New-File (Join-Path $mod 'Braindance3D-Settings.ini') "[Log]`r`nLevel = 1`r`n" }
    if (-not $NoGame) { New-File (Join-Path $game 'bin\x64\Cyberpunk2077.exe') 'MZ not the game' }
    if (-not $NoEngineIni) { New-File (Join-Path $game 'engine\config\platform\pc\braindance3d.ini') "[Odyssey3D]`r`n" }
    return $mod
}

# A stand-in for Samsung's Odyssey 3D Hub: <Root>\<Name>\GameLauncherData\core\Odyssey3DSDK.dll with no
# version resource and no Braindance3D-Settings.ini, so the installer must treat it as foreign.
function New-ForeignSdk {
    param([string]$Root, [string]$Name = 'Odyssey 3D Hub')
    $dir = Join-Path $Root $Name
    New-File (Join-Path $dir 'GameLauncherData\core\Odyssey3DSDK.dll') 'MZ Samsung stand-in for tests'
    return [System.IO.Path]::GetFullPath($dir)
}

# A drive letter that doesn't exist (for "InstallPath on a removed drive").
function Get-MissingDrivePath {
    param([string]$Rest = 'Odyssey 3D Hub')
    $used = @(Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | ForEach-Object { $_.Name.ToUpperInvariant() })
    foreach ($letter in 'QRSTUVWXYJKLMNOP'.ToCharArray()) {
        $l = [string]$letter
        if ($used -notcontains $l -and -not [System.IO.Directory]::Exists("${l}:\")) { return "${l}:\$Rest" }
    }
    return $null
}

# ---------------------------------------------------------------- registry (Windows only)

function Assert-TestKey {
    param([string]$Key)
    if (-not $Key.StartsWith($script:TestKeyRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to touch '$Key': tests may only use keys under $($script:TestKeyRoot)"
    }
}

# Returns a throwaway key path HKCU:\SOFTWARE\Braindance3D-Tests\<guid>\Samsung\Odyssey3DHub (not created).
# The <guid>\Samsung level lets tests check how far up uninstall removes empty keys.
function New-TestKey {
    $key = '{0}\{1}\Samsung\Odyssey3DHub' -f $script:TestKeyRoot, [guid]::NewGuid().ToString('N')
    Assert-TestKey $key
    return $key
}

function Get-TestKeyBase {
    param([string]$Key)
    Assert-TestKey $Key
    $rest = $Key.Substring($script:TestKeyRoot.Length + 1)
    return $script:TestKeyRoot + '\' + $rest.Split('\')[0]
}

function Remove-TestKey {
    param([string]$Key)
    if (-not $Key -or -not $script:HasRegistry) { return }
    $base = Get-TestKeyBase $Key
    if (Test-Path -LiteralPath $base) { Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue }
    if ((Test-Path -LiteralPath $script:TestKeyRoot) -and
        ((Get-Item -LiteralPath $script:TestKeyRoot).SubKeyCount -eq 0) -and
        ((Get-Item -LiteralPath $script:TestKeyRoot).ValueCount -eq 0)) {
        Remove-Item -LiteralPath $script:TestKeyRoot -Force -ErrorAction SilentlyContinue
    }
}

# Creates the key (and parents) and sets string values.
function Set-TestValues {
    param([string]$Key, [hashtable]$Values)
    Assert-TestKey $Key
    if (-not (Test-Path -LiteralPath $Key)) { New-Item -Path $Key -Force | Out-Null }
    foreach ($name in $Values.Keys) { New-ItemProperty -LiteralPath $Key -Name $name -Value $Values[$name] -PropertyType String -Force | Out-Null }
}

# All values of a key as one string "name=value; name=value" sorted by name ('' when the key is missing),
# for before/after comparisons.
function Get-TestValues {
    param([string]$Key)
    Assert-TestKey $Key
    if (-not (Test-Path -LiteralPath $Key)) { return '' }
    $item = Get-Item -LiteralPath $Key
    $out = @()
    foreach ($name in ($item.GetValueNames() | Sort-Object)) { $out += ('{0}={1}' -f $name, $item.GetValue($name)) }
    return ($out -join '; ')
}

function Get-TestValue {
    param([string]$Key, [string]$Name)
    Assert-TestKey $Key
    if (-not (Test-Path -LiteralPath $Key)) { return $null }
    return (Get-Item -LiteralPath $Key).GetValue($Name)
}

# ---------------------------------------------------------------- running the script

# The PowerShell that runs these tests, for running the setup script as its own process.
function Get-CurrentPowerShell {
    return (Get-Process -Id $PID).Path
}

# Runs Braindance3D-Setup.ps1 like the .bat files do (-File ... -NoPause) and returns the exit code.
# Output goes to $OutFile when given.
function Invoke-SetupProcess {
    param([string]$Exe, [string]$Script, [string[]]$Arguments, [string]$OutFile)
    $all = @('-NoProfile', '-NonInteractive')
    if ($script:IsWin) { $all += @('-ExecutionPolicy', 'Bypass') }
    $all += @('-File', $Script) + $Arguments
    # Windows PowerShell 5.1 turns a child's stderr into error records, which throw under 'Stop'.
    $ErrorActionPreference = 'Continue'
    if ($OutFile) { & $Exe @all *> $OutFile } else { & $Exe @all *> $null }
    return $LASTEXITCODE
}

# The installer's $script:SdkRelativePath etc. come from dot-sourcing the script; this makes sure a
# result is a single [int] (an array would break "exit $code").
function Assert-Code {
    param($Result, [int]$Expected)
    @($Result).Count | Should -Be 1
    $Result | Should -BeOfType [int]
    $Result | Should -Be $Expected
}
