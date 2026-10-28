# Pester 5 tests for install / uninstall / status of Braindance3D-Setup.ps1 against a throwaway registry key.
# Windows only: on other systems every test here is skipped (or, with BD3D_FAKE_REGISTRY=1, runs against an
# in-memory fake of HKCU for development; see FakeRegistry.ps1).
#
# Every test uses its own key HKCU:\SOFTWARE\Braindance3D-Tests\<guid>\Samsung\Odyssey3DHub (see
# TestHelpers.ps1), removed afterwards, and passes -SamsungDir explicitly, so neither the real
# HKCU\SOFTWARE\Samsung key nor a real "C:\Program Files\Odyssey 3D Hub" is ever looked at.

BeforeDiscovery {
    $IsWin = [System.Environment]::OSVersion.Platform -eq 'Win32NT'
    $HasRegistry = $IsWin -or ($env:BD3D_FAKE_REGISTRY -eq '1')   # see FakeRegistry.ps1
}

BeforeAll {
    . (Join-Path $PSScriptRoot 'TestHelpers.ps1')
    . $script:SetupScript
    $script:Root = New-TestRoot
    $script:NoSamsung = Join-Path $script:Root 'no-samsung-here'
}

AfterAll {
    Restore-TestRoot $script:Root
}

Describe 'Registry' -Skip:(-not $HasRegistry) {
    BeforeEach {
        $script:LogFile = $null
        $script:Key = New-TestKey
        $script:Base = Get-TestKeyBase $script:Key
        $script:Parent = $script:Key.Substring(0, $script:Key.LastIndexOf('\'))
        Mock Write-Host {}
    }
    AfterEach {
        Remove-TestKey $script:Key
    }

    Context 'Invoke-Install' {
        It 'fresh install creates the key and all values' {
            $mod = New-ModFolder -Root $script:Root -Name 'fresh'
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $mod
            Get-TestValue $script:Key 'Braindance3D_InstallPath' | Should -BeExactly $mod
            Get-TestValue $script:Key 'Braindance3D_InstalledBy' | Should -BeExactly $script:Version
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly '<none>'
            (Get-Item -LiteralPath $script:Key).ValueCount | Should -Be 4
            (Get-Item -LiteralPath $script:Key).GetValueKind('InstallPath') | Should -Be 'String'
        }

        It 'stores the folder without a trailing slash' {
            $mod = New-ModFolder -Root $script:Root -Name 'slash'
            Assert-Code (Invoke-Install -Key $script:Key -Dir ($mod + '\') -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $mod
        }

        It 'reinstall from the same folder keeps the saved previous value and updates the version' {
            $mod = New-ModFolder -Root $script:Root -Name 'again'
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-again'
            Set-TestValues $script:Key @{ InstallPath = $mod; Braindance3D_InstallPath = $mod; Braindance3D_InstalledBy = '0.0.1'; Braindance3D_PreviousInstallPath = $hub }
            $sameFolder = $mod
            if ($script:IsWin) { $sameFolder = $mod.ToUpperInvariant() + '\' }   # file names are case-insensitive there
            Assert-Code (Invoke-Install -Key $script:Key -Dir $sameFolder -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly $hub
            Get-TestValue $script:Key 'Braindance3D_InstalledBy' | Should -BeExactly $script:Version
            Test-SamePath (Get-TestValue $script:Key 'InstallPath') $mod | Should -BeTrue
        }

        It 'reinstall without bookkeeping values (InstallPath set by hand) works' {
            $mod = New-ModFolder -Root $script:Root -Name 'byhand'
            Set-TestValues $script:Key @{ InstallPath = $mod }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly '<none>'
        }

        It 'install over another copy of this mod keeps what the first install saved' {
            $a = New-ModFolder -Root $script:Root -Name 'copy A'
            $b = New-ModFolder -Root $script:Root -Name 'copy B'
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-AB'
            Set-TestValues $script:Key @{ InstallPath = $hub }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $a -Force -SamsungDir $script:NoSamsung) 0
            Assert-Code (Invoke-Install -Key $script:Key -Dir $b -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $b
            Get-TestValue $script:Key 'Braindance3D_InstallPath' | Should -BeExactly $b
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly $hub
            # ...and uninstall from B still restores Samsung's value.
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $b) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $hub
        }

        It 'install over another copy found only by its settings file (no bookkeeping)' {
            $a = New-ModFolder -Root $script:Root -Name 'manual A'
            $b = New-ModFolder -Root $script:Root -Name 'manual B'
            Set-TestValues $script:Key @{ InstallPath = $a }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $b -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $b
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly '<none>'
        }

        It 'install over a removed copy of this mod (InstallPath is the path we wrote, folder gone)' {
            $gone = Join-Path $script:Root 'removed copy\bin\x64\Braindance3D'
            $mod = New-ModFolder -Root $script:Root -Name 'new copy'
            Set-TestValues $script:Key @{ InstallPath = $gone; Braindance3D_InstallPath = $gone; Braindance3D_InstalledBy = '0.1.0'; Braindance3D_PreviousInstallPath = '<none>' }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $mod
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly '<none>'
        }

        It 'install over a stale InstallPath (folder gone, not ours) saves it for uninstall' {
            $stale = Join-Path $script:Root 'old Odyssey 3D Hub'
            $mod = New-ModFolder -Root $script:Root -Name 'over stale'
            Set-TestValues $script:Key @{ InstallPath = $stale; Version = '1.2.3' }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $mod
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly $stale
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $stale
            Get-TestValues $script:Key | Should -BeExactly "InstallPath=$stale; Version=1.2.3"
        }

        It 'refuses to replace a working foreign SDK (code 3) and changes nothing' {
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-refuse'
            $mod = New-ModFolder -Root $script:Root -Name 'refused'
            Set-TestValues $script:Key @{ InstallPath = $hub; SamsungValue = 'keep' }
            $before = Get-TestValues $script:Key
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 3
            Get-TestValues $script:Key | Should -BeExactly $before
        }

        It 'refuses even when stale bookkeeping values of this mod are present' {
            # E.g. Samsung's Hub re-registered itself after a forced install.
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-rereg'
            $mod = New-ModFolder -Root $script:Root -Name 'rereg'
            Set-TestValues $script:Key @{ InstallPath = $hub; Braindance3D_InstallPath = $mod; Braindance3D_InstalledBy = '0.1.0'; Braindance3D_PreviousInstallPath = $hub }
            $before = Get-TestValues $script:Key
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 3
            Get-TestValues $script:Key | Should -BeExactly $before
        }

        It '-Force replaces a foreign SDK and uninstall restores it' {
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-force'
            $mod = New-ModFolder -Root $script:Root -Name 'forced'
            Set-TestValues $script:Key @{ InstallPath = $hub; SamsungValue = 'keep' }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -Force -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $mod
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly $hub
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Get-TestValues $script:Key | Should -BeExactly "InstallPath=$hub; SamsungValue=keep"
        }

        It 'refuses (code 3) when Samsung''s default folder has a foreign SDK and there is no InstallPath' {
            $samsung = New-ForeignSdk -Root $script:Root -Name 'Program Files\Odyssey 3D Hub'
            $mod = New-ModFolder -Root $script:Root -Name 'default hub'
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $samsung) 3
            Test-Path -LiteralPath $script:Key | Should -BeFalse
            Test-Path -LiteralPath $script:Parent | Should -BeFalse
        }

        It '-Force overrides Samsung''s default folder; uninstall removes InstallPath again' {
            $samsung = New-ForeignSdk -Root $script:Root -Name 'PF\Odyssey 3D Hub'
            $mod = New-ModFolder -Root $script:Root -Name 'default hub forced'
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -Force -SamsungDir $samsung) 0
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly '<none>'
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Test-Path -LiteralPath $script:Key | Should -BeFalse   # the game falls back to Samsung's folder again
        }

        It 'does not refuse when Samsung''s default folder holds a copy of this mod' {
            $samsungLike = New-ModFolder -Root $script:Root -Name 'mod in default dir'
            $mod = New-ModFolder -Root $script:Root -Name 'default ours'
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $samsungLike) 0
        }

        It 'refuses (code 2) when the SDK DLL is missing' {
            $mod = New-ModFolder -Root $script:Root -Name 'no dll' -NoDll
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 2
            Test-Path -LiteralPath $script:Key | Should -BeFalse
        }

        It 'refuses (code 6) outside a game folder, and -AnyFolder allows it with a warning' {
            $mod = New-ModFolder -Root $script:Root -Name 'staging' -NoGame
            Mock Write-Log {}
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 6
            Test-Path -LiteralPath $script:Key | Should -BeFalse
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -AnyFolder -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $mod
            Should -Invoke Write-Log -ParameterFilter { $Level -eq 'WARN' } -Scope It
        }

        It 'refuses (code 5) inside %TEMP% (started from inside the zip)' -Skip:(-not $IsWin) {
            $mod = New-ModFolder -Root $script:Root -Name 'in temp'
            $saved = $env:TEMP
            try {
                $env:TEMP = Join-Path $script:Root 'in temp'
                Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 5
                $env:TEMP = (Join-Path $script:Root 'IN TEMP\bin') + '\'
                Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 5
            } finally {
                $env:TEMP = $saved
            }
            Test-Path -LiteralPath $script:Key | Should -BeFalse
        }

        It 'warns about an old game, a missing engine ini and a running game but still installs' {
            $mod = New-ModFolder -Root $script:Root -Name 'warnings'
            Mock Write-Log {}
            Mock Test-IsElevated { $false }
            $script:FakeGame = [pscustomobject]@{ ExePath = 'x'; Found = $true; ProductVersion = '2.5'; Running = $false; IniPath = 'y'; IniFound = $true }
            Mock Get-GameInfo { $script:FakeGame }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Should -Invoke Write-Log -ParameterFilter { $Level -eq 'WARN' } -Times 0 -Exactly -Scope It
            $cases = @(
                [pscustomobject]@{ ExePath = 'x'; Found = $true; ProductVersion = '2.12'; Running = $false; IniPath = 'y'; IniFound = $true },
                [pscustomobject]@{ ExePath = 'x'; Found = $true; ProductVersion = '2.5'; Running = $false; IniPath = 'y'; IniFound = $false },
                [pscustomobject]@{ ExePath = 'x'; Found = $true; ProductVersion = '2.5'; Running = $true; IniPath = 'y'; IniFound = $true }
            )
            $n = 0
            foreach ($case in $cases) {
                $n++
                $script:FakeGame = $case
                Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
                Should -Invoke Write-Log -ParameterFilter { $Level -eq 'WARN' } -Times $n -Exactly -Scope It
            }
        }

        It 'warns when run as administrator but still installs' {
            $mod = New-ModFolder -Root $script:Root -Name 'admin'
            Mock Write-Log {}
            Mock Test-IsElevated { $true }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Should -Invoke Write-Log -ParameterFilter { $Level -eq 'WARN' } -Scope It
        }

        It 'returns 4 when the value does not stick' {
            $mod = New-ModFolder -Root $script:Root -Name 'nostick'
            Mock Set-ItemProperty {} -ParameterFilter { $Name -eq 'InstallPath' }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 4
        }

        It 'writes the bookkeeping first, so an interrupted install can be undone' {
            $mod = New-ModFolder -Root $script:Root -Name 'interrupted'
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-interrupted'
            Set-TestValues $script:Key @{ InstallPath = $hub }
            Mock Set-ItemProperty { throw 'simulated failure' } -ParameterFilter { $Name -eq 'InstallPath' }
            Assert-Code (Invoke-Main -Action install -Key $script:Key -Dir $mod -Force -SamsungDir $script:NoSamsung) 1
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly $hub
            Get-TestValue $script:Key 'Braindance3D_InstallPath' | Should -BeExactly $mod
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $hub
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Get-TestValues $script:Key | Should -BeExactly "InstallPath=$hub"
        }

        It 'install and uninstall with spaces, brackets, parentheses and non-ASCII in the path' {
            $mod = New-ModFolder -Root $script:Root -Name ('Program Files (x86)\GOG Galaxy\Games\Cyberpunk 2077 [GOG] ' + $script:NonAscii)
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $mod
            (Show-Status -Key $script:Key -Dir $mod).Installed | Should -BeTrue
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Test-Path -LiteralPath $script:Key | Should -BeFalse
        }

        It 'leaves an InstallPath on a drive that is not there alone (code 3); -Force replaces it' {
            $missing = Get-MissingDrivePath
            if (-not $missing) { Set-ItResult -Skipped -Because 'no free drive letter'; return }
            $mod = New-ModFolder -Root $script:Root -Name 'missing drive'
            Set-TestValues $script:Key @{ InstallPath = $missing }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 3
            Get-TestValues $script:Key | Should -BeExactly "InstallPath=$missing"
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung -Force) 0
            Get-TestValue $script:Key 'Braindance3D_PreviousInstallPath' | Should -BeExactly $missing
        }

        It 'leaves a network InstallPath alone (code 3)' {
            $mod = New-ModFolder -Root $script:Root -Name 'unc'
            Set-TestValues $script:Key @{ InstallPath = '\\nas\apps\Odyssey 3D Hub' }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 3
            Get-TestValues $script:Key | Should -BeExactly 'InstallPath=\\nas\apps\Odyssey 3D Hub'
        }

        It 'refuses a stale InstallPath when Samsung''s default folder has an SDK (code 3)' {
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-default-stale'
            $stale = Join-Path $script:Root 'gone Hub'
            $mod = New-ModFolder -Root $script:Root -Name 'stale with samsung'
            Set-TestValues $script:Key @{ InstallPath = $stale }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $hub) 3
            Get-TestValues $script:Key | Should -BeExactly "InstallPath=$stale"
        }

        It 'refuses an InstallPath that is not a text value (code 3), even with -Force' {
            $mod = New-ModFolder -Root $script:Root -Name 'dword'
            if (-not (Test-Path -LiteralPath $script:Key)) { New-Item -Path $script:Key -Force | Out-Null }
            New-ItemProperty -LiteralPath $script:Key -Name InstallPath -Value 7 -PropertyType DWord | Out-Null
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 3
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung -Force) 3
            Get-TestValue $script:Key 'Braindance3D_InstallPath' | Should -BeNullOrEmpty
        }
    }

    Context 'Invoke-Uninstall' {
        It 'warns that the RED4ext plugin registers the mod again' {
            $mod = New-ModFolder -Root $script:Root -Name 'u-red4ext'
            $plugin = Get-Red4extPluginPath $mod
            New-File $plugin 'MZ plugin stand-in'
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Mock Write-Log {}
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Should -Invoke Write-Log -ParameterFilter { $Level -eq 'WARN' -and $Message -match 'RED4ext plugin is still installed' }
        }

        It 'does nothing when the key is missing' {
            $mod = New-ModFolder -Root $script:Root -Name 'u-none'
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Test-Path -LiteralPath $script:Key | Should -BeFalse
            Test-Path -LiteralPath $script:Base | Should -BeFalse
        }

        It 'removes everything it installed, then the empty key and its empty parent, but nothing above' {
            $mod = New-ModFolder -Root $script:Root -Name 'u-here'
            New-Item -Path $script:Base -Force | Out-Null   # stands for HKCU\SOFTWARE
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Test-Path -LiteralPath $script:Key | Should -BeFalse
            Test-Path -LiteralPath $script:Parent | Should -BeFalse
            Test-Path -LiteralPath $script:Base | Should -BeTrue
        }

        It 'keeps a parent that has other values or subkeys' {
            $mod = New-ModFolder -Root $script:Root -Name 'u-parent'
            Set-TestValues $script:Parent @{ Other = 'x' }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Test-Path -LiteralPath $script:Key | Should -BeFalse
            Get-TestValues $script:Parent | Should -BeExactly "Other=x"

            New-Item -Path ($script:Parent + '\Monitor') -Force | Out-Null
            Remove-ItemProperty -LiteralPath $script:Parent -Name Other
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Test-Path -LiteralPath ($script:Parent + '\Monitor') | Should -BeTrue
        }

        It 'keeps the key when it has values of its own' {
            $mod = New-ModFolder -Root $script:Root -Name 'u-own'
            Set-TestValues $script:Key @{ Version = '1.0' }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Get-TestValues $script:Key | Should -BeExactly "Version=1.0"
        }

        It 'leaves a foreign SDK alone but removes leftover bookkeeping' {
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-u'
            $mod = New-ModFolder -Root $script:Root -Name 'u-foreign'
            Set-TestValues $script:Key @{ InstallPath = $hub; Braindance3D_InstallPath = $mod; Braindance3D_InstalledBy = '0.1.0'; Braindance3D_PreviousInstallPath = '<none>' }
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Get-TestValues $script:Key | Should -BeExactly "InstallPath=$hub"
        }

        It 'changes nothing for a foreign SDK without bookkeeping' {
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-u2'
            $mod = New-ModFolder -Root $script:Root -Name 'u-foreign2'
            Set-TestValues $script:Key @{ InstallPath = $hub; Version = '2' }
            $before = Get-TestValues $script:Key
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Get-TestValues $script:Key | Should -BeExactly $before
        }

        It 'works after the mod files were deleted (run from another copy)' {
            $a = New-ModFolder -Root $script:Root -Name 'u-deleted A'
            $b = New-ModFolder -Root $script:Root -Name 'u-deleted B'
            Assert-Code (Invoke-Install -Key $script:Key -Dir $a -SamsungDir $script:NoSamsung) 0
            Remove-Item -LiteralPath (Join-Path $script:Root 'u-deleted A') -Recurse -Force
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $b) 0
            Test-Path -LiteralPath $script:Key | Should -BeFalse
        }

        It 'works after the mod files were deleted (run for the deleted folder itself)' {
            $a = New-ModFolder -Root $script:Root -Name 'u-deleted C'
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-deleted'
            Set-TestValues $script:Key @{ InstallPath = $hub }
            Assert-Code (Invoke-Install -Key $script:Key -Dir $a -Force -SamsungDir $script:NoSamsung) 0
            Remove-Item -LiteralPath (Join-Path $script:Root 'u-deleted C') -Recurse -Force
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $a) 0
            Get-TestValues $script:Key | Should -BeExactly "InstallPath=$hub"
        }

        It 'treats an InstallPath that holds a copy of this mod as ours' {
            $a = New-ModFolder -Root $script:Root -Name 'u-copy'
            Set-TestValues $script:Key @{ InstallPath = $a }
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir (Join-Path $script:Root 'elsewhere')) 0
            Test-Path -LiteralPath $script:Key | Should -BeFalse
        }

        It 'removes bookkeeping when InstallPath itself is gone' {
            $mod = New-ModFolder -Root $script:Root -Name 'u-noinstallpath'
            Set-TestValues $script:Key @{ Braindance3D_InstallPath = $mod; Braindance3D_InstalledBy = '0.1.0'; Braindance3D_PreviousInstallPath = '<none>' }
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Test-Path -LiteralPath $script:Key | Should -BeFalse
        }

        It 'is idempotent' {
            $mod = New-ModFolder -Root $script:Root -Name 'u-twice'
            Assert-Code (Invoke-Install -Key $script:Key -Dir $mod -SamsungDir $script:NoSamsung) 0
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Test-Path -LiteralPath $script:Key | Should -BeFalse
        }

        It 'leaves an InstallPath on a drive that no longer exists alone' {
            $missing = Get-MissingDrivePath
            if (-not $missing) { Set-ItResult -Skipped -Because 'no free drive letter'; return }
            $mod = New-ModFolder -Root $script:Root -Name 'u-missing drive'
            Set-TestValues $script:Key @{ InstallPath = $missing }
            Assert-Code (Invoke-Uninstall -Key $script:Key -Dir $mod) 0
            Get-TestValues $script:Key | Should -BeExactly "InstallPath=$missing"
        }
    }

    Context 'Show-Status' {
        It 'reports installed when InstallPath is this folder (any case or trailing slash)' {
            $mod = New-ModFolder -Root $script:Root -Name 's-here'
            Set-TestValues $script:Key @{ InstallPath = ($mod.ToUpperInvariant() + '\') }
            $s = Show-Status -Key $script:Key -Dir $mod
            @($s).Count | Should -Be 1
            $s.Installed | Should -BeTrue
            $s.Installed | Should -BeOfType [bool]
            $s.Registry.KeyExists | Should -BeTrue
            $s.Sdk.Exists | Should -BeTrue
            $s.Game.Found | Should -BeTrue
        }
        It 'reports not installed when InstallPath is elsewhere' {
            $mod = New-ModFolder -Root $script:Root -Name 's-elsewhere'
            $hub = New-ForeignSdk -Root $script:Root -Name 'Hub-s'
            Set-TestValues $script:Key @{ InstallPath = $hub }
            Mock Write-Log {}
            (Show-Status -Key $script:Key -Dir $mod).Installed | Should -BeFalse
            Should -Invoke Write-Log -ParameterFilter { $Level -eq 'WARN' } -Scope It
        }
        It 'reports not installed without a key, and does not create it' {
            $mod = New-ModFolder -Root $script:Root -Name 's-none'
            $s = Show-Status -Key $script:Key -Dir $mod
            $s.Installed | Should -BeFalse
            $s.Registry.KeyExists | Should -BeFalse
            Test-Path -LiteralPath $script:Key | Should -BeFalse
        }
        It 'changes nothing in the registry' {
            $mod = New-ModFolder -Root $script:Root -Name 's-ro'
            Set-TestValues $script:Key @{ InstallPath = $mod; Braindance3D_InstalledBy = '0.0.1' }
            $before = Get-TestValues $script:Key
            Show-Status -Key $script:Key -Dir $mod | Out-Null
            Get-TestValues $script:Key | Should -BeExactly $before
        }
    }

    Context 'registry helpers' {
        It 'Get-RegistryValue returns strings and $null for missing values' {
            Set-TestValues $script:Key @{ InstallPath = 'C:\x' }
            New-ItemProperty -LiteralPath $script:Key -Name Number -Value 5 -PropertyType DWord | Out-Null
            Get-RegistryValue $script:Key 'InstallPath' | Should -BeExactly 'C:\x'
            Get-RegistryValue $script:Key 'Number' | Should -BeExactly '<not a text value>'
            Get-RegistryValue $script:Key 'Missing' | Should -BeNullOrEmpty
            Get-RegistryValue ($script:Key + '\Nope') 'InstallPath' | Should -BeNullOrEmpty
        }
        It 'Get-RegistryState reads all values' {
            Set-TestValues $script:Key @{ InstallPath = 'C:\a'; Braindance3D_InstalledBy = 'v'; Braindance3D_InstallPath = 'C:\b'; Braindance3D_PreviousInstallPath = 'C:\c' }
            $s = Get-RegistryState $script:Key
            $s.KeyExists | Should -BeTrue
            $s.InstallPath | Should -BeExactly 'C:\a'
            $s.Marker | Should -BeExactly 'v'
            $s.OwnPath | Should -BeExactly 'C:\b'
            $s.Previous | Should -BeExactly 'C:\c'
        }
        It 'Remove-Bookkeeping removes only the three Braindance3D_* values' {
            Set-TestValues $script:Key @{ InstallPath = 'C:\a'; Braindance3D_InstalledBy = 'v'; Braindance3D_InstallPath = 'C:\b'; Braindance3D_PreviousInstallPath = 'C:\c'; Other = 'o' }
            Remove-Bookkeeping $script:Key
            Get-TestValues $script:Key | Should -BeExactly "InstallPath=C:\a; Other=o"
            { Remove-Bookkeeping $script:Key } | Should -Not -Throw   # already gone
        }
        It 'Remove-EmptyKey removes an empty key and an empty parent, nothing more' {
            New-Item -Path $script:Key -Force | Out-Null
            Remove-EmptyKey $script:Key
            Test-Path -LiteralPath $script:Key | Should -BeFalse
            Test-Path -LiteralPath $script:Parent | Should -BeFalse
            Test-Path -LiteralPath $script:Base | Should -BeTrue
        }
        It 'Remove-EmptyKey keeps a key with a value, and then does not look at the parent' {
            Set-TestValues $script:Key @{ A = '1' }
            Remove-EmptyKey $script:Key
            Test-Path -LiteralPath $script:Key | Should -BeTrue
            Test-Path -LiteralPath $script:Parent | Should -BeTrue
        }
        It 'Remove-EmptyKey keeps a key with a subkey' {
            New-Item -Path ($script:Key + '\Sub') -Force | Out-Null
            Remove-EmptyKey $script:Key
            Test-Path -LiteralPath ($script:Key + '\Sub') | Should -BeTrue
        }
        It 'Remove-EmptyKey does nothing for a missing key' {
            { Remove-EmptyKey $script:Key } | Should -Not -Throw
        }
    }
}
