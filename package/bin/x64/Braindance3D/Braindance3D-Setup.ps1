<#
.SYNOPSIS
    3D Braindance Edition: registers or unregisters the Odyssey 3D SDK stand-in.

.DESCRIPTION
    Cyberpunk 2077 looks for Samsung's Odyssey 3D SDK through the per-user registry value
    HKCU\SOFTWARE\Samsung\Odyssey3DHub\InstallPath. "install" points that value at this folder,
    which holds GameLauncherData\core\Odyssey3DSDK.dll. "uninstall" puts it back the way it was.
    No admin rights are needed. Only that one registry key is changed: besides InstallPath it holds
    three Braindance3D_* values that let uninstall restore what was there before. The key (and an
    empty HKCU\SOFTWARE\Samsung) is created by install and removed by uninstall when nothing else is in it.

    Every step is logged to Braindance3D-setup.log in this folder
    (or %LOCALAPPDATA%\Braindance3D if this folder is read-only).

.PARAMETER Action
    install, uninstall or status.

.PARAMETER Force
    install only: replace Samsung's Odyssey 3D Hub (or another SDK). Its setting is saved and
    uninstall restores it. Run the uninstaller before deleting the mod, or it can't be restored.

.PARAMETER AnyFolder
    install only: allow a folder that isn't <game>\bin\x64\Braindance3D (for example a mod manager's
    staging folder). The game then loads the DLL from that folder.

.PARAMETER NoPause
    Don't wait for Enter at the end.
#>
[CmdletBinding()]
param(
    [ValidateSet('install', 'uninstall', 'status')]
    [string]$Action = 'status',
    [switch]$Force,
    [switch]$AnyFolder,
    [switch]$NoPause,
    # Internal, for the test suite: the registry key, install folder, log file and Samsung's default folder.
    [string]$RegistryPath = 'HKCU:\SOFTWARE\Samsung\Odyssey3DHub',
    [string]$InstallDir = '',
    [string]$LogPath = '',
    [string]$SamsungDefaultDir = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:Version = '0.1.1'
$script:ProductName = '3D Braindance Edition'
$script:SettingsFile = 'Braindance3D-Settings.ini'
$script:MarkerName = 'Braindance3D_InstalledBy'
$script:OwnPathName = 'Braindance3D_InstallPath'
$script:PreviousName = 'Braindance3D_PreviousInstallPath'
$script:NoPreviousValue = '<none>'
$script:SdkRelativePath = 'GameLauncherData\core\Odyssey3DSDK.dll'
$script:LogFile = $null

# ------------------------------------------------------------------ logging

function Initialize-Log {
    param([string]$Preferred, [string]$Dir)
    $candidates = @()
    if ($Preferred) { $candidates += $Preferred }
    if ($Dir) { $candidates += (Join-Path $Dir 'Braindance3D-setup.log') }
    if ($env:LOCALAPPDATA) { $candidates += (Join-Path $env:LOCALAPPDATA 'Braindance3D\Braindance3D-setup.log') }
    foreach ($candidate in $candidates) {
        try {
            $parent = Split-Path -Parent $candidate
            if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
            Add-Content -LiteralPath $candidate -Value '' -Encoding UTF8
            $script:LogFile = $candidate
            return $candidate
        } catch {
            continue
        }
    }
    $script:LogFile = $null
    return $null
}

function Write-Log {
    param([string]$Message, [ValidateSet('INFO', 'WARN', 'ERROR', 'OK')][string]$Level = 'INFO')
    $line = '{0} [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $Level, $Message
    if ($script:LogFile) {
        try { Add-Content -LiteralPath $script:LogFile -Value $line -Encoding UTF8 } catch { }
    }
    $color = switch ($Level) { 'WARN' { 'Yellow' } 'ERROR' { 'Red' } 'OK' { 'Green' } default { 'Gray' } }
    Write-Host $Message -ForegroundColor $color
}

# ------------------------------------------------------------------ helpers

# Full path without a trailing slash. Plain string handling if .NET calls are blocked (Constrained Language Mode).
function Get-NormalizedPath {
    param([string]$Path)
    if (-not $Path) { return '' }
    $full = $Path
    try { $full = [System.IO.Path]::GetFullPath($Path) } catch { }
    $trimmed = $full.TrimEnd('\', '/')
    if ($trimmed -match '^[A-Za-z]:$') { return $trimmed + '\' }
    return $trimmed
}

function Test-SamePath {
    param([string]$A, [string]$B)
    if (-not $A -or -not $B) { return $false }
    return ((Get-NormalizedPath $A) -ieq (Get-NormalizedPath $B))
}

# $true when $Path is $Parent or inside it.
function Test-PathUnder {
    param([string]$Path, [string]$Parent)
    if (-not $Path -or -not $Parent) { return $false }
    $p = (Get-NormalizedPath $Path).TrimEnd('\') + '\'
    $q = (Get-NormalizedPath $Parent).TrimEnd('\') + '\'
    return $p.ToLowerInvariant().StartsWith($q.ToLowerInvariant())
}

function Get-ParentPath {
    param([string]$Path)
    if (-not $Path) { return '' }
    $parent = Split-Path -Parent $Path
    if ($null -eq $parent) { return '' }
    return [string]$parent
}

function Get-RegistryValue {
    param([string]$Key, [string]$Name)
    if (-not (Test-Path -LiteralPath $Key)) { return $null }
    $item = Get-ItemProperty -LiteralPath $Key -ErrorAction SilentlyContinue
    if ($null -eq $item) { return $null }
    $prop = $item.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $null }
    return [string]$prop.Value
}

function Get-SdkPath {
    param([string]$Dir)
    # Plain string join: Join-Path throws when the drive doesn't exist (a stale InstallPath).
    return ($Dir.TrimEnd('\', '/') + '\' + $script:SdkRelativePath)
}

# Describes the SDK DLL in a folder: whether it exists and whether it is this mod's.
function Get-SdkInfo {
    param([string]$Dir)
    $info = [ordered]@{ Dir = $Dir; DllPath = ''; Exists = $false; IsOurs = $false; Product = ''; Version = '' }
    if (-not $Dir) { return [pscustomobject]$info }
    $dll = Get-SdkPath $Dir
    $info.DllPath = $dll
    if (Test-Path -LiteralPath $dll -PathType Leaf) {
        $info.Exists = $true
        try {
            $vi = (Get-Item -LiteralPath $dll).VersionInfo
            $info.Product = [string]$vi.ProductName
            $info.Version = [string]$vi.ProductVersion
        } catch { }
        if ($info.Product -eq $script:ProductName) { $info.IsOurs = $true }
        if (Test-Path -LiteralPath ($Dir.TrimEnd('\', '/') + '\' + $script:SettingsFile) -PathType Leaf) { $info.IsOurs = $true }
    }
    return [pscustomobject]$info
}

function Get-RegistryState {
    param([string]$Key)
    $state = [ordered]@{
        KeyExists   = (Test-Path -LiteralPath $Key)
        InstallPath = (Get-RegistryValue $Key 'InstallPath')
        Marker      = (Get-RegistryValue $Key $script:MarkerName)
        OwnPath     = (Get-RegistryValue $Key $script:OwnPathName)
        Previous    = (Get-RegistryValue $Key $script:PreviousName)
    }
    return [pscustomobject]$state
}

# $true when the registry's InstallPath is this mod's: the path an install of this mod wrote,
# or a folder that holds a copy of this mod.
function Test-InstallPathIsOurs {
    param($Reg)
    if (-not $Reg.InstallPath) { return $false }
    if ($Reg.OwnPath -and (Test-SamePath $Reg.InstallPath $Reg.OwnPath)) { return $true }
    return (Get-SdkInfo $Reg.InstallPath).IsOurs
}

# Finds the game next to this folder (<game>\bin\x64\Braindance3D) and reports its version.
function Get-GameInfo {
    param([string]$Dir)
    $info = [ordered]@{ ExePath = ''; Found = $false; ProductVersion = ''; Running = $false; IniPath = ''; IniFound = $false }
    $binX64 = Get-ParentPath (Get-NormalizedPath $Dir)
    if ($binX64) {
        $exe = Join-Path $binX64 'Cyberpunk2077.exe'
        $info.ExePath = $exe
        if (Test-Path -LiteralPath $exe -PathType Leaf) {
            $info.Found = $true
            try { $info.ProductVersion = [string](Get-Item -LiteralPath $exe).VersionInfo.ProductVersion } catch { }
        }
        $gameRoot = Get-ParentPath (Get-ParentPath $binX64)
        if ($gameRoot) {
            $ini = Join-Path $gameRoot 'engine\config\platform\pc\braindance3d.ini'
            $info.IniPath = $ini
            $info.IniFound = (Test-Path -LiteralPath $ini -PathType Leaf)
        }
    }
    $info.Running = [bool](Get-Process -Name 'Cyberpunk2077' -ErrorAction SilentlyContinue)
    return [pscustomobject]$info
}

# Cyberpunk versions are decimals (2.12 is older than 2.2, which is older than 2.5).
# Returns $true when $Have >= $Need; a missing or unparsable version counts as $true.
function Test-MinimumVersion {
    param([string]$Have, [string]$Need)
    $m = [regex]::Match([string]$Have, '^\s*(\d+(\.\d+)?)')
    if (-not $m.Success) { return $true }
    $n = [regex]::Match([string]$Need, '^\s*(\d+(\.\d+)?)')
    try {
        $inv = [System.Globalization.CultureInfo]::InvariantCulture
        return ([decimal]::Parse($m.Groups[1].Value, $inv) -ge [decimal]::Parse($n.Groups[1].Value, $inv))
    } catch {
        return $true
    }
}

# Samsung's default install folder, which the game uses when there is no InstallPath value.
function Get-SamsungDefaultDir {
    param([string]$Override)
    if ($Override) { return $Override }
    if ($env:ProgramFiles) { return (Join-Path $env:ProgramFiles 'Odyssey 3D Hub') }
    return ''
}

function Test-IsElevated {
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch {
        return $false
    }
}

# Removes the key, then its parent, if they hold no values and no subkeys.
function Remove-EmptyKey {
    param([string]$Key)
    foreach ($k in @($Key, (Get-ParentPath $Key))) {
        if (-not $k -or -not (Test-Path -LiteralPath $k)) { continue }
        $item = Get-Item -LiteralPath $k
        if ($item.ValueCount -eq 0 -and $item.SubKeyCount -eq 0) {
            Remove-Item -LiteralPath $k
            Write-Log "Removed empty registry key $k"
        } else {
            break
        }
    }
}

function Remove-Bookkeeping {
    param([string]$Key)
    foreach ($name in @($script:MarkerName, $script:OwnPathName, $script:PreviousName)) {
        Remove-ItemProperty -LiteralPath $Key -Name $name -ErrorAction SilentlyContinue
    }
}

# ------------------------------------------------------------------ actions

function Show-Status {
    param([string]$Key, [string]$Dir)
    $reg = Get-RegistryState $Key
    $own = Get-SdkInfo $Dir
    $game = Get-GameInfo $Dir
    Write-Log "$script:ProductName setup $script:Version"
    Write-Log "This folder: $Dir"
    Write-Log ("SDK stand-in here: {0} (exists={1}, ours={2}, version={3})" -f $own.DllPath, $own.Exists, $own.IsOurs, $own.Version)
    Write-Log ("Game: {0} (found={1}, version={2}, running={3})" -f $game.ExePath, $game.Found, $game.ProductVersion, $game.Running)
    Write-Log ("Engine ini: {0} (found={1})" -f $game.IniPath, $game.IniFound)
    Write-Log ("Registry {0}: exists={1} InstallPath='{2}' installedBy='{3}' ownPath='{4}' previous='{5}'" -f $Key, $reg.KeyExists, $reg.InstallPath, $reg.Marker, $reg.OwnPath, $reg.Previous)
    $active = [bool]($reg.InstallPath -and (Test-SamePath $reg.InstallPath $Dir))
    if ($active) { Write-Log 'Status: INSTALLED. The game will load the stand-in from this folder.' 'OK' }
    elseif ($reg.InstallPath) { Write-Log "Status: NOT INSTALLED from this folder. The game uses the SDK at '$($reg.InstallPath)'." 'WARN' }
    else { Write-Log 'Status: NOT INSTALLED.' 'WARN' }
    return [pscustomobject]@{ Installed = $active; Registry = $reg; Sdk = $own; Game = $game }
}

function Invoke-Install {
    param([string]$Key, [string]$Dir, [switch]$Force, [switch]$AnyFolder, [string]$SamsungDir)
    $Dir = Get-NormalizedPath $Dir
    Write-Log "Installing $script:ProductName $script:Version from $Dir"

    if ($env:TEMP -and (Test-PathUnder $Dir $env:TEMP)) {
        Write-Log "This folder is inside the temporary folder ($env:TEMP), probably because the installer was started from inside the zip. Extract the zip into the game folder first, then run the installer from there." 'ERROR'
        return 5
    }

    $own = Get-SdkInfo $Dir
    if (-not $own.Exists) {
        Write-Log "The SDK stand-in is missing: $($own.DllPath). Re-extract the mod and try again." 'ERROR'
        return 2
    }
    Write-Log "Found the SDK stand-in: $($own.DllPath) (product '$($own.Product)', version '$($own.Version)')"

    $game = Get-GameInfo $Dir
    if ($game.Found) {
        Write-Log "Found the game: $($game.ExePath) (version '$($game.ProductVersion)')"
        if (-not (Test-MinimumVersion $game.ProductVersion '2.5')) {
            Write-Log "This game version is older than 2.5, which has no 3D Mode. The mod does nothing until you update." 'WARN'
        }
        if (-not $game.IniFound) {
            Write-Log "The engine ini is missing: $($game.IniPath). Without it the game interlaces for an Odyssey monitor instead of drawing side by side. Re-extract the mod." 'WARN'
        }
    } elseif ($AnyFolder) {
        Write-Log "Cyberpunk2077.exe isn't next to this folder ($($game.ExePath)). Continuing because -AnyFolder was given." 'WARN'
    } else {
        Write-Log "Cyberpunk2077.exe isn't next to this folder (looked for $($game.ExePath)). Extract the zip into the game folder (the one with 'bin' and 'engine'), so this folder is <game>\bin\x64\Braindance3D, then run the installer from there. Nothing was changed." 'ERROR'
        return 6
    }
    if ($game.Running) {
        Write-Log 'Cyberpunk 2077 is running. Restart it after this so it picks up the change.' 'WARN'
    }
    if (Test-IsElevated) {
        Write-Log 'This is running as administrator. The setting is written for the account running the installer; if that is a different admin account, the game won''t see it. Run the installer without admin rights.' 'WARN'
    }

    $reg = Get-RegistryState $Key
    Write-Log ("Registry before: exists={0} InstallPath='{1}' installedBy='{2}' ownPath='{3}' previous='{4}'" -f $reg.KeyExists, $reg.InstallPath, $reg.Marker, $reg.OwnPath, $reg.Previous)

    # Decide what uninstall should put back, and whether replacing the current setting is allowed.
    $previous = $script:NoPreviousValue
    if ($reg.InstallPath -and (Test-InstallPathIsOurs $reg)) {
        # Reinstall or another copy of this mod: keep what the first install saved.
        if ($reg.Previous) { $previous = $reg.Previous }
        if (-not (Test-SamePath $reg.InstallPath $Dir)) { Write-Log "Replacing another copy of this mod at '$($reg.InstallPath)'." }
    } elseif ($reg.InstallPath) {
        $other = Get-SdkInfo $reg.InstallPath
        if (-not $other.Exists) {
            Write-Log "The current InstallPath '$($reg.InstallPath)' has no SDK DLL, so nothing uses it. Saving it so uninstall can put it back."
        } elseif ($Force) {
            Write-Log "Replacing the SDK at '$($reg.InstallPath)' because -Force was given. Uninstall restores it: run Uninstall-Braindance3D.bat BEFORE you delete this mod." 'WARN'
        } else {
            Write-Log "Another Odyssey 3D SDK is installed at '$($reg.InstallPath)' (product '$($other.Product)'). That is probably Samsung's Odyssey 3D Hub, which a real Odyssey 3D monitor needs." 'ERROR'
            Write-Log 'Nothing was changed. To replace it anyway, open a command prompt in this folder and run: Install-Braindance3D.bat -Force (Uninstall-Braindance3D.bat restores it).' 'ERROR'
            return 3
        }
        $previous = $reg.InstallPath
    } else {
        $samsung = Get-SamsungDefaultDir $SamsungDir
        if ($samsung -and (Get-SdkInfo $samsung).Exists -and -not (Get-SdkInfo $samsung).IsOurs) {
            if (-not $Force) {
                Write-Log "Samsung's Odyssey 3D Hub is installed in '$samsung', which a real Odyssey 3D monitor needs. Nothing was changed. To replace it anyway, open a command prompt in this folder and run: Install-Braindance3D.bat -Force (Uninstall-Braindance3D.bat restores it)." 'ERROR'
                return 3
            }
            Write-Log "Overriding Samsung's Odyssey 3D Hub in '$samsung' because -Force was given. Uninstall restores it: run Uninstall-Braindance3D.bat BEFORE you delete this mod." 'WARN'
        }
    }

    if (-not (Test-Path -LiteralPath $Key)) {
        New-Item -Path $Key -Force | Out-Null
        Write-Log "Created registry key $Key"
    }
    # Bookkeeping first, so an interrupted install can still be undone.
    Set-ItemProperty -LiteralPath $Key -Name $script:PreviousName -Value $previous
    Set-ItemProperty -LiteralPath $Key -Name $script:OwnPathName -Value $Dir
    Set-ItemProperty -LiteralPath $Key -Name $script:MarkerName -Value $script:Version
    Set-ItemProperty -LiteralPath $Key -Name 'InstallPath' -Value $Dir

    $after = Get-RegistryState $Key
    Write-Log ("Registry after: InstallPath='{0}' installedBy='{1}' ownPath='{2}' previous='{3}'" -f $after.InstallPath, $after.Marker, $after.OwnPath, $after.Previous)
    if (-not (Test-SamePath $after.InstallPath $Dir)) {
        Write-Log 'The registry value did not stick. Please report this with the log.' 'ERROR'
        return 4
    }
    Write-Log 'Installed. Start the game, turn on 3D Mode in the video settings, then view the screen in half side-by-side (half-SBS) mode.' 'OK'
    return 0
}

function Invoke-Uninstall {
    param([string]$Key, [string]$Dir)
    $Dir = Get-NormalizedPath $Dir
    Write-Log "Uninstalling $script:ProductName $script:Version (this folder: $Dir)"
    $reg = Get-RegistryState $Key
    Write-Log ("Registry before: exists={0} InstallPath='{1}' installedBy='{2}' ownPath='{3}' previous='{4}'" -f $reg.KeyExists, $reg.InstallPath, $reg.Marker, $reg.OwnPath, $reg.Previous)

    if (-not $reg.KeyExists) {
        Write-Log 'Nothing to undo: the registry key does not exist.' 'OK'
        return 0
    }

    $ours = (Test-InstallPathIsOurs $reg) -or ($reg.InstallPath -and (Test-SamePath $reg.InstallPath $Dir))
    if ($reg.InstallPath -and -not $ours) {
        Write-Log "InstallPath points at another SDK ('$($reg.InstallPath)'), not this mod. Leaving it alone." 'WARN'
    } elseif ($reg.InstallPath) {
        if ($reg.Previous -and $reg.Previous -ne $script:NoPreviousValue) {
            Set-ItemProperty -LiteralPath $Key -Name 'InstallPath' -Value $reg.Previous
            Write-Log "Restored the previous InstallPath '$($reg.Previous)'."
        } else {
            Remove-ItemProperty -LiteralPath $Key -Name 'InstallPath'
            Write-Log 'Removed InstallPath.'
        }
    }
    if ($reg.Marker -or $reg.OwnPath -or $reg.Previous) {
        Remove-Bookkeeping $Key
        Write-Log 'Removed this mod''s bookkeeping values.'
    }
    Remove-EmptyKey $Key

    $after = Get-RegistryState $Key
    Write-Log ("Registry after: exists={0} InstallPath='{1}'" -f $after.KeyExists, $after.InstallPath)
    Write-Log 'Uninstalled. The game no longer loads the stand-in. Now delete the mod files (or remove the mod in your mod manager).' 'OK'
    return 0
}

function Invoke-Main {
    param([string]$Action, [string]$Key, [string]$Dir, [switch]$Force, [switch]$AnyFolder, [string]$Log, [string]$SamsungDir)
    Initialize-Log -Preferred $Log -Dir $Dir | Out-Null
    Write-Log "==== $script:ProductName setup $script:Version, action '$Action', PowerShell $($PSVersionTable.PSVersion) ($($ExecutionContext.SessionState.LanguageMode)), log '$script:LogFile'"
    try {
        switch ($Action) {
            'install' { return (Invoke-Install -Key $Key -Dir $Dir -Force:$Force -AnyFolder:$AnyFolder -SamsungDir $SamsungDir) }
            'uninstall' { return (Invoke-Uninstall -Key $Key -Dir $Dir) }
            default { Show-Status -Key $Key -Dir $Dir | Out-Null; return 0 }
        }
    } catch {
        Write-Log ("Failed: {0} ({1})" -f $_.Exception.Message, $_.InvocationInfo.PositionMessage) 'ERROR'
        return 1
    }
}

# Run unless dot-sourced (the tests dot-source this file to call the functions directly).
if ($MyInvocation.InvocationName -ne '.') {
    # The script's own folder, resolved here: $PSScriptRoot can be empty in a param default.
    if (-not $InstallDir) { $InstallDir = $PSScriptRoot }
    if (-not $InstallDir -and $PSCommandPath) { $InstallDir = Split-Path -Parent $PSCommandPath }
    if (-not $InstallDir -and $MyInvocation.MyCommand.Path) { $InstallDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
    $code = Invoke-Main -Action $Action -Key $RegistryPath -Dir $InstallDir -Force:$Force -AnyFolder:$AnyFolder -Log $LogPath -SamsungDir $SamsungDefaultDir
    if ($script:LogFile) { Write-Host "Log: $script:LogFile" }
    if (-not $NoPause) {
        Write-Host ''
        Read-Host 'Press Enter to close' | Out-Null
    }
    exit $code
}
