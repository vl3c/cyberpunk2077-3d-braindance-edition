# Pester 5 tests for the helper functions of Braindance3D-Setup.ps1 that don't touch the registry.
# Run with tests/Run-PesterTests.ps1. Most run on any OS; a few need Windows path semantics.

BeforeDiscovery {
    $IsWin = [System.Environment]::OSVersion.Platform -eq 'Win32NT'
}

BeforeAll {
    . (Join-Path $PSScriptRoot 'TestHelpers.ps1')
    . $script:SetupScript
    $script:Root = New-TestRoot
    Mock Write-Host {}
}

AfterAll {
    Restore-TestRoot $script:Root
}

Describe 'Get-NormalizedPath' {
    It 'returns an empty string for empty input' {
        Get-NormalizedPath '' | Should -BeExactly ''
        Get-NormalizedPath $null | Should -BeExactly ''
    }
    It 'trims trailing separators' {
        $p = Join-Path $script:Root 'a'
        Get-NormalizedPath ($p + '\') | Should -Be (Get-NormalizedPath $p)
        Get-NormalizedPath ($p + '/') | Should -Be (Get-NormalizedPath $p)
        Get-NormalizedPath ($p + '\\') | Should -Be (Get-NormalizedPath $p)
    }
    It 'makes the path absolute' {
        [System.IO.Path]::IsPathRooted((Get-NormalizedPath 'relative\x')) | Should -BeTrue
    }
    It 'keeps a drive root as D:\' -Skip:(-not $IsWin) {
        Get-NormalizedPath 'D:\' | Should -BeExactly 'D:\'
        Get-NormalizedPath 'D:\\' | Should -BeExactly 'D:\'
        Get-NormalizedPath 'd:/' | Should -BeExactly 'd:\'
    }
    It 'resolves . and .. segments' -Skip:(-not $IsWin) {
        Get-NormalizedPath 'C:\Games\.\Cyberpunk\..\Cyberpunk 2077\' | Should -BeExactly 'C:\Games\Cyberpunk 2077'
    }
    It 'returns a string, not an array' {
        @(Get-NormalizedPath 'C:\x').Count | Should -Be 1
        Get-NormalizedPath 'C:\x' | Should -BeOfType [string]
    }
}

Describe 'Test-SamePath' {
    It 'ignores case and trailing slashes' {
        Test-SamePath 'C:\Games\Cyberpunk 2077\bin\x64\Braindance3D' 'c:\games\CYBERPUNK 2077\bin\x64\braindance3d\' | Should -BeTrue
        Test-SamePath 'C:\Games\X\' 'C:\Games\X' | Should -BeTrue
    }
    It 'is false for different folders' {
        Test-SamePath 'C:\Games\X' 'C:\Games\X2' | Should -BeFalse
        Test-SamePath 'C:\Games\X' 'C:\Games' | Should -BeFalse
    }
    It 'is false when either side is empty' {
        Test-SamePath '' 'C:\x' | Should -BeFalse
        Test-SamePath 'C:\x' '' | Should -BeFalse
        Test-SamePath $null $null | Should -BeFalse
    }
    It 'handles brackets, spaces and non-ASCII' {
        $a = 'C:\Games [GOG]\' + $script:NonAscii + '\x'
        Test-SamePath $a ($a.ToUpperInvariant() + '\') | Should -BeTrue
    }
    It 'normalises . and .. on Windows' -Skip:(-not $IsWin) {
        Test-SamePath 'C:\Games\.\a\..\X' 'C:\Games\X' | Should -BeTrue
    }
    It 'returns a single boolean' {
        $r = Test-SamePath 'C:\a' 'C:\a'
        @($r).Count | Should -Be 1
        $r | Should -BeOfType [bool]
    }
}

Describe 'Test-PathUnder' {
    It 'is true for the folder itself and anything inside it' {
        Test-PathUnder 'C:\Users\me\AppData\Local\Temp' 'C:\Users\me\AppData\Local\Temp' | Should -BeTrue
        Test-PathUnder 'C:\Users\me\AppData\Local\Temp\Rar$EXa1.234\bin\x64\Braindance3D' 'C:\Users\me\AppData\Local\Temp' | Should -BeTrue
        Test-PathUnder 'C:\Users\me\AppData\Local\Temp\x' 'C:\Users\me\AppData\Local\Temp\' | Should -BeTrue
    }
    It 'ignores case' {
        Test-PathUnder 'c:\USERS\me\appdata\local\temp\x' 'C:\Users\me\AppData\Local\Temp' | Should -BeTrue
    }
    It 'does not match a sibling that only shares a prefix' {
        Test-PathUnder 'C:\Temp2\mod' 'C:\Temp' | Should -BeFalse
        Test-PathUnder 'C:\TempFolder' 'C:\Temp' | Should -BeFalse
    }
    It 'is false for a parent of the folder' {
        Test-PathUnder 'C:\Users' 'C:\Users\me' | Should -BeFalse
    }
    It 'is false when either side is empty' {
        Test-PathUnder '' 'C:\Temp' | Should -BeFalse
        Test-PathUnder 'C:\Temp' '' | Should -BeFalse
    }
}

Describe 'Get-ParentPath' {
    It 'returns an empty string for empty input' {
        Get-ParentPath '' | Should -BeExactly ''
    }
    It 'returns the parent folder' -Skip:(-not $IsWin) {
        Get-ParentPath 'C:\Games\Cyberpunk 2077\bin' | Should -BeExactly 'C:\Games\Cyberpunk 2077'
        Get-ParentPath 'C:\Games' | Should -BeExactly 'C:\'
    }
    It 'returns an empty string above a drive root' {
        Get-ParentPath 'C:\' | Should -BeExactly ''
    }
    It 'returns the parent folder (any OS)' {
        $p = Join-Path $script:Root 'child'
        Get-ParentPath $p | Should -Be (Split-Path -Parent $p)
    }
}

Describe 'Test-MinimumVersion' {
    It '<Have> >= <Need> is <Expected>' -TestCases @(
        @{ Have = '2.5'; Need = '2.5'; Expected = $true }
        @{ Have = '2.51'; Need = '2.5'; Expected = $true }
        @{ Have = '2.6'; Need = '2.5'; Expected = $true }
        @{ Have = '3.0'; Need = '2.5'; Expected = $true }
        @{ Have = '3'; Need = '2.5'; Expected = $true }
        @{ Have = '2.5.1'; Need = '2.5'; Expected = $true }
        @{ Have = '3.0.88.46235'; Need = '2.5'; Expected = $true }
        @{ Have = ' 2.6 '; Need = '2.5'; Expected = $true }
        @{ Have = '2.4'; Need = '2.5'; Expected = $false }
        @{ Have = '2.12'; Need = '2.5'; Expected = $false }   # Cyberpunk versions are decimals: 2.12 < 2.2 < 2.5
        @{ Have = '2.21'; Need = '2.5'; Expected = $false }
        @{ Have = '2.2'; Need = '2.12'; Expected = $true }
        @{ Have = '2'; Need = '2.5'; Expected = $false }
        @{ Have = '1.63'; Need = '2.5'; Expected = $false }
        @{ Have = '0'; Need = '2.5'; Expected = $false }
    ) {
        Test-MinimumVersion $Have $Need | Should -Be $Expected
    }
    It 'counts a missing or unparsable version as new enough: <Have>' -TestCases @(
        @{ Have = '' }
        @{ Have = $null }
        @{ Have = 'v2.0' }
        @{ Have = 'unknown' }
        @{ Have = '.5' }
    ) {
        Test-MinimumVersion $Have '2.5' | Should -BeTrue
    }
    It 'does not throw on huge numbers' {
        { Test-MinimumVersion '99999999999999999999999999999999999.1' '2.5' } | Should -Not -Throw
        Test-MinimumVersion '99999999999999999999999999999999999.1' '2.5' | Should -BeTrue
    }
    It 'does not depend on the current culture (decimal comma)' {
        $saved = [System.Threading.Thread]::CurrentThread.CurrentCulture
        try {
            [System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::GetCultureInfo('de-DE')
            Test-MinimumVersion '2.12' '2.5' | Should -BeFalse
            Test-MinimumVersion '2.6' '2.5' | Should -BeTrue
        } finally {
            [System.Threading.Thread]::CurrentThread.CurrentCulture = $saved
        }
    }
    It 'returns a single boolean' {
        $r = Test-MinimumVersion '2.5' '2.5'
        @($r).Count | Should -Be 1
        $r | Should -BeOfType [bool]
    }
}

Describe 'Get-SdkPath and Get-SdkInfo' {
    It 'Get-SdkPath points at GameLauncherData\core\Odyssey3DSDK.dll' {
        $p = Get-SdkPath (Join-Path $script:Root 'x')
        $p.Replace('/', '\') | Should -BeLike '*\x\GameLauncherData\core\Odyssey3DSDK.dll'
    }
    It 'describes an empty folder argument' {
        $i = Get-SdkInfo ''
        $i.Exists | Should -BeFalse
        $i.IsOurs | Should -BeFalse
        $i.DllPath | Should -BeExactly ''
    }
    It 'reports a missing DLL' {
        $mod = New-ModFolder -Root $script:Root -Name 'nodll' -NoDll
        $i = Get-SdkInfo $mod
        $i.Exists | Should -BeFalse
        $i.IsOurs | Should -BeFalse
    }
    It 'reports a missing folder' {
        $i = Get-SdkInfo (Join-Path $script:Root 'does-not-exist')
        $i.Exists | Should -BeFalse
        $i.IsOurs | Should -BeFalse
    }
    It 'treats a DLL without version info and without our settings file as foreign' {
        $hub = New-ForeignSdk -Root $script:Root -Name 'Hub1'
        $i = Get-SdkInfo $hub
        $i.Exists | Should -BeTrue
        $i.IsOurs | Should -BeFalse
        $i.Product | Should -BeExactly ''
    }
    It 'recognises this mod by its settings file' {
        $mod = New-ModFolder -Root $script:Root -Name 'ours-by-ini'
        (Get-SdkInfo $mod).IsOurs | Should -BeTrue
    }
    It 'does not count the settings file without a DLL' {
        $mod = New-ModFolder -Root $script:Root -Name 'ini-only' -NoDll
        (Get-SdkInfo $mod).IsOurs | Should -BeFalse
    }
    It 'recognises the built DLL by its ProductName even without the settings file' -Skip:(-not $IsWin) {
        if (-not $script:BuiltDll) { Set-ItResult -Skipped -Because 'build/Odyssey3DSDK.dll is missing (run ./build.sh)'; return }
        $mod = New-ModFolder -Root $script:Root -Name 'ours-by-dll' -RealDll -NoSettings
        $i = Get-SdkInfo $mod
        $i.Product | Should -BeExactly $script:ProductName
        $i.Version | Should -BeExactly (Get-VersionH)
        $i.IsOurs | Should -BeTrue
    }
    It 'works with brackets, spaces and non-ASCII in the folder name' {
        $mod = New-ModFolder -Root $script:Root -Name ('Games [GOG] (x86) ' + $script:NonAscii)
        $i = Get-SdkInfo $mod
        $i.Exists | Should -BeTrue
        $i.IsOurs | Should -BeTrue
    }
    It 'does not throw for an InstallPath on a drive that does not exist' {
        $missing = Get-MissingDrivePath
        if (-not $missing) { Set-ItResult -Skipped -Because 'no free drive letter'; return }
        { Get-SdkInfo $missing } | Should -Not -Throw
        (Get-SdkInfo $missing).Exists | Should -BeFalse
    }
}

Describe 'Test-InstallPathIsOurs' {
    It 'is false without an InstallPath' {
        Test-InstallPathIsOurs ([pscustomobject]@{ InstallPath = $null; OwnPath = 'C:\x' }) | Should -BeFalse
        Test-InstallPathIsOurs ([pscustomobject]@{ InstallPath = ''; OwnPath = '' }) | Should -BeFalse
    }
    It 'is true when InstallPath is the path this mod wrote, even if that folder is gone' {
        $gone = Join-Path $script:Root 'deleted-copy'
        Test-InstallPathIsOurs ([pscustomobject]@{ InstallPath = $gone; OwnPath = ($gone.ToUpperInvariant() + '\') }) | Should -BeTrue
    }
    It 'is true for a folder holding a copy of this mod' {
        $mod = New-ModFolder -Root $script:Root -Name 'copy-b'
        Test-InstallPathIsOurs ([pscustomobject]@{ InstallPath = $mod; OwnPath = $null }) | Should -BeTrue
    }
    It 'is false for a foreign SDK even when the marker values exist' {
        $hub = New-ForeignSdk -Root $script:Root -Name 'Hub2'
        Test-InstallPathIsOurs ([pscustomobject]@{ InstallPath = $hub; OwnPath = (Join-Path $script:Root 'elsewhere') }) | Should -BeFalse
    }
    It 'is false for a folder that is gone and was not written by this mod' {
        Test-InstallPathIsOurs ([pscustomobject]@{ InstallPath = (Join-Path $script:Root 'gone'); OwnPath = $null }) | Should -BeFalse
    }
}

Describe 'Get-GameInfo' {
    It 'finds the game and the engine ini around game\bin\x64\Braindance3D' {
        $mod = New-ModFolder -Root $script:Root -Name 'Game A'
        Mock Get-Process { $null }
        $g = Get-GameInfo $mod
        $g.Found | Should -BeTrue
        $g.ExePath.Replace('/', '\') | Should -BeLike '*\Game A\bin\x64\Cyberpunk2077.exe'
        $g.IniFound | Should -BeTrue
        $g.IniPath.Replace('/', '\') | Should -BeLike '*\Game A\engine\config\platform\pc\braindance3d.ini'
        $g.Running | Should -BeFalse
        $g.ProductVersion | Should -BeExactly ''   # the dummy exe has no version resource
    }
    It 'reports a missing game and a missing engine ini' {
        $mod = New-ModFolder -Root $script:Root -Name 'Not A Game' -NoGame -NoEngineIni
        $g = Get-GameInfo $mod
        $g.Found | Should -BeFalse
        $g.IniFound | Should -BeFalse
    }
    It 'reports a running game' {
        $mod = New-ModFolder -Root $script:Root -Name 'Game B'
        Mock Get-Process { [pscustomobject]@{ Name = 'Cyberpunk2077'; Id = 1 } } -ParameterFilter { $Name -eq 'Cyberpunk2077' }
        (Get-GameInfo $mod).Running | Should -BeTrue
    }
    It 'copes with a folder near the drive root' -Skip:(-not $IsWin) {
        { Get-GameInfo 'C:\Braindance3D' } | Should -Not -Throw
        (Get-GameInfo 'C:\Braindance3D').IniPath | Should -BeExactly ''
    }
    It 'works with brackets and non-ASCII in the game path' {
        $mod = New-ModFolder -Root $script:Root -Name ('Cyberpunk 2077 [GOG] ' + $script:NonAscii)
        $g = Get-GameInfo $mod
        $g.Found | Should -BeTrue
        $g.IniFound | Should -BeTrue
    }
}

Describe 'Get-SamsungDefaultDir' {
    It 'uses the override when given' {
        Get-SamsungDefaultDir 'X:\Hub' | Should -BeExactly 'X:\Hub'
    }
    It 'uses %ProgramFiles%\Odyssey 3D Hub otherwise, preferring the 64-bit %ProgramW6432%' {
        $saved = $env:ProgramFiles
        $saved64 = $env:ProgramW6432
        try {
            if (Test-Path Env:\ProgramW6432) { Remove-Item Env:\ProgramW6432 }
            $env:ProgramFiles = $script:Root
            (Get-SamsungDefaultDir '').Replace('/', '\') | Should -BeLike '*\Odyssey 3D Hub'
            $env:ProgramW6432 = Join-Path $script:Root 'pf64'
            (Get-SamsungDefaultDir '').Replace('/', '\') | Should -BeLike '*\pf64\Odyssey 3D Hub'
            Remove-Item Env:\ProgramW6432
            Remove-Item Env:\ProgramFiles
            Get-SamsungDefaultDir '' | Should -BeExactly ''
        } finally {
            $env:ProgramFiles = $saved
            if ($saved64) { $env:ProgramW6432 = $saved64 }
        }
    }
}

Describe 'Test-PathIsCheckable' {
    It 'accepts a local path on a drive that is there' {
        Test-PathIsCheckable (Join-Path $script:Root 'gone') | Should -BeTrue
    }
    It 'rejects network, quoted and empty paths' {
        Test-PathIsCheckable '\\server\share\Odyssey 3D Hub' | Should -BeFalse
        Test-PathIsCheckable '"C:\Program Files\Odyssey 3D Hub"' | Should -BeFalse
        Test-PathIsCheckable '' | Should -BeFalse
        Test-PathIsCheckable 'relative\path' | Should -BeFalse
    }
    It 'rejects a drive that is not there' {
        $missing = Get-MissingDrivePath
        if (-not $missing) { Set-ItResult -Skipped -Because 'no free drive letter'; return }
        Test-PathIsCheckable $missing | Should -BeFalse
    }
}

Describe 'Get-Red4extPluginPath' {
    It 'finds the plugin DLL under the game folder' {
        if (-not $IsWin) { Set-ItResult -Skipped -Because 'Windows paths'; return }
        $mod = New-ModFolder -Root $script:Root -Name 'r4e path'
        (Get-Red4extPluginPath $mod).Replace('/', '\') | Should -BeLike '*\r4e path\red4ext\plugins\Braindance3D\Braindance3D.dll'
    }
    It 'returns an empty string for an empty folder' {
        Get-Red4extPluginPath '' | Should -BeExactly ''
    }
}

Describe 'Test-IsElevated' {
    It 'returns a boolean and never throws' {
        $r = Test-IsElevated
        @($r).Count | Should -Be 1
        $r | Should -BeOfType [bool]
    }
}

Describe 'Get-RegistryValue and Get-RegistryState without a key' {
    It 'return nothing for a key that does not exist' {
        $key = 'HKCU:\SOFTWARE\Braindance3D-Tests\' + [guid]::NewGuid().ToString('N')
        Get-RegistryValue $key 'InstallPath' | Should -BeNullOrEmpty
        $s = Get-RegistryState $key
        $s.KeyExists | Should -BeFalse
        $s.InstallPath | Should -BeNullOrEmpty
        $s.Marker | Should -BeNullOrEmpty
        $s.OwnPath | Should -BeNullOrEmpty
        $s.Previous | Should -BeNullOrEmpty
    }
}
