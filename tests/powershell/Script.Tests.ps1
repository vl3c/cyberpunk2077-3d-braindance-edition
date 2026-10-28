# Pester 5 tests for Braindance3D-Setup.ps1 as a whole: logging, Invoke-Main, running it as a process the
# way the .bat files do, and consistency of the files that ship together. Registry-dependent cases are
# skipped outside Windows.

BeforeDiscovery {
    $IsWin = [System.Environment]::OSVersion.Platform -eq 'Win32NT'
    $WinPs = ''
    if ($IsWin -and $env:SystemRoot) {
        $p = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        if (Test-Path -LiteralPath $p) { $WinPs = $p }
    }
    # The PowerShells to run the script with as a process: this one, plus Windows PowerShell 5.1 (what the .bat uses).
    $Hosts = @(@{ Exe = (Get-Process -Id $PID).Path; HostName = 'current' })
    if ($WinPs -and ($WinPs -ne (Get-Process -Id $PID).Path)) { $Hosts += @{ Exe = $WinPs; HostName = 'powershell.exe' } }
}

BeforeAll {
    . (Join-Path $PSScriptRoot 'TestHelpers.ps1')
    . $script:SetupScript
    $script:Root = New-TestRoot
    $script:NoSamsung = Join-Path $script:Root 'no-samsung-here'
    Mock Write-Host {}
}

AfterAll {
    Restore-TestRoot $script:Root
}

Describe 'Initialize-Log' {
    BeforeEach { $script:LogFile = $null }

    It 'uses the preferred file and creates its folder (brackets, non-ASCII)' {
        $p = Join-Path (Join-Path $script:Root ('logs [1] ' + $script:NonAscii)) 'setup.log'
        $r = Initialize-Log -Preferred $p -Dir (Join-Path $script:Root 'unused')
        $r | Should -BeExactly $p
        $script:LogFile | Should -BeExactly $p
        Test-Path -LiteralPath $p | Should -BeTrue
    }
    It 'falls back to Braindance3D-setup.log in the folder' {
        $dir = Join-Path $script:Root 'logdir'
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
        $r = Initialize-Log -Preferred '' -Dir $dir
        $r | Should -BeExactly (Join-Path $dir 'Braindance3D-setup.log')
    }
    It 'falls back to %LOCALAPPDATA%\Braindance3D when the folder is not writable' {
        $blocker = Join-Path $script:Root 'a-file-not-a-folder'
        New-File $blocker 'x'
        $r = Initialize-Log -Preferred (Join-Path $blocker 'x.log') -Dir $blocker
        $r | Should -BeExactly (Join-Path $env:LOCALAPPDATA 'Braindance3D\Braindance3D-setup.log')
        Test-Path -LiteralPath $r | Should -BeTrue
    }
    It 'returns $null when nothing is writable, and logging still works' {
        $blocker = Join-Path $script:Root 'another-file'
        New-File $blocker 'x'
        $saved = $env:LOCALAPPDATA
        try {
            $env:LOCALAPPDATA = $blocker
            Initialize-Log -Preferred (Join-Path $blocker 'x.log') -Dir $blocker | Should -BeNullOrEmpty
            $script:LogFile | Should -BeNullOrEmpty
            { Write-Log 'no file' 'WARN' } | Should -Not -Throw
        } finally {
            $env:LOCALAPPDATA = $saved
        }
    }
    It 'appends to an existing log instead of replacing it' {
        $p = Join-Path $script:Root 'append.log'
        New-File $p "earlier run`r`n"
        Initialize-Log -Preferred $p -Dir '' | Out-Null
        Write-Log 'later run'
        $text = Get-Content -LiteralPath $p -Raw
        $text | Should -Match 'earlier run'
        $text | Should -Match 'later run'
    }
}

Describe 'Write-Log' {
    It 'writes the message and its level to the log file' {
        $p = Join-Path $script:Root 'write.log'
        Initialize-Log -Preferred $p -Dir '' | Out-Null
        Write-Log ('message with ' + $script:NonAscii) 'ERROR'
        $text = [System.IO.File]::ReadAllText($p, [System.Text.Encoding]::UTF8)
        $text | Should -Match ([regex]::Escape('message with ' + $script:NonAscii))
        $text | Should -Match 'ERROR'
    }
    It 'accepts the four levels and rejects others' {
        foreach ($l in 'INFO', 'WARN', 'ERROR', 'OK') { { Write-Log 'x' $l } | Should -Not -Throw }
        { Write-Log 'x' 'DEBUG' } | Should -Throw
    }
    It 'does not throw when the log file has gone away' {
        $script:LogFile = Join-Path $script:Root 'gone\gone.log'
        { Write-Log 'x' } | Should -Not -Throw
        $script:LogFile = $null
    }
    It 'writes nothing to the output stream' {
        $script:LogFile = $null
        $out = Write-Log 'x' 'OK'
        $out | Should -BeNullOrEmpty
    }
}

Describe 'Invoke-Main' {
    BeforeEach { $script:LogFile = $null }

    It 'status returns 0 and writes the setup log in the folder' {
        $mod = New-ModFolder -Root $script:Root -Name 'main-status'
        $key = 'HKCU:\SOFTWARE\Braindance3D-Tests\' + [guid]::NewGuid().ToString('N')
        Assert-Code (Invoke-Main -Action status -Key $key -Dir $mod) 0
        Test-Path -LiteralPath (Join-Path $mod 'Braindance3D-setup.log') | Should -BeTrue
        if ($script:IsWin) { Test-Path -LiteralPath $key | Should -BeFalse }
    }
    It 'passes install''s code through as a single int' {
        Mock Invoke-Install { 6 }
        Assert-Code (Invoke-Main -Action install -Key 'HKCU:\SOFTWARE\Braindance3D-Tests\x' -Dir $script:Root -SamsungDir $script:NoSamsung) 6
        Should -Invoke Invoke-Install -Times 1 -Exactly
    }
    It 'passes -Force, -AnyFolder and -SamsungDir on to install' {
        Mock Invoke-Install { 0 }
        Invoke-Main -Action install -Key 'K' -Dir $script:Root -Force -AnyFolder -SamsungDir 'S' | Out-Null
        Should -Invoke Invoke-Install -Times 1 -Exactly -ParameterFilter { $Force -and $AnyFolder -and $SamsungDir -eq 'S' -and $Key -eq 'K' }
    }
    It 'passes uninstall''s code through' {
        Mock Invoke-Uninstall { 0 }
        Assert-Code (Invoke-Main -Action uninstall -Key 'K' -Dir $script:Root) 0
        Should -Invoke Invoke-Uninstall -Times 1 -Exactly
    }
    It 'returns 1 and logs the error when something throws' {
        Mock Invoke-Uninstall { throw 'simulated failure' }
        Mock Write-Log {}
        Assert-Code (Invoke-Main -Action uninstall -Key 'K' -Dir $script:Root) 1
        Should -Invoke Write-Log -ParameterFilter { $Level -eq 'ERROR' -and $Message -match 'simulated failure' }
    }
    It 'install of a folder without the DLL returns 2 before touching the registry' {
        $mod = New-ModFolder -Root $script:Root -Name 'main-nodll' -NoDll
        Mock Get-RegistryState { throw 'registry must not be read' }
        Assert-Code (Invoke-Main -Action install -Key 'HKCU:\SOFTWARE\Braindance3D-Tests\x' -Dir $mod -SamsungDir $script:NoSamsung) 2
    }
    It 'install outside a game folder returns 6 before touching the registry' {
        $mod = New-ModFolder -Root $script:Root -Name 'main-nogame' -NoGame
        Mock Get-RegistryState { throw 'registry must not be read' }
        Assert-Code (Invoke-Main -Action install -Key 'HKCU:\SOFTWARE\Braindance3D-Tests\x' -Dir $mod -SamsungDir $script:NoSamsung) 6
    }
}

Describe 'Running the script as a process (like the .bat files)' {
    BeforeEach {
        $script:Key = $null
        if ($script:IsWin) { $script:Key = New-TestKey }
    }
    AfterEach {
        Remove-TestKey $script:Key
    }

    It 'status exits 0 (<HostName>)' -TestCases $Hosts {
        $mod = New-ModFolder -Root $script:Root -Name ('proc status ' + $HostName)
        $key = 'HKCU:\SOFTWARE\Braindance3D-Tests\' + [guid]::NewGuid().ToString('N')
        $code = Invoke-SetupProcess -Exe $Exe -Script (Join-Path $mod 'Braindance3D-Setup.ps1') -Arguments @('-Action', 'status', '-NoPause', '-RegistryPath', $key)
        $code | Should -Be 0
        Test-Path -LiteralPath (Join-Path $mod 'Braindance3D-setup.log') | Should -BeTrue
    }

    It 'install without the DLL exits 2 (<HostName>)' -TestCases $Hosts {
        $mod = New-ModFolder -Root $script:Root -Name ('proc nodll ' + $HostName) -NoDll
        $code = Invoke-SetupProcess -Exe $Exe -Script (Join-Path $mod 'Braindance3D-Setup.ps1') -Arguments @('-Action', 'install', '-NoPause', '-RegistryPath', 'HKCU:\SOFTWARE\Braindance3D-Tests\x', '-SamsungDefaultDir', $script:NoSamsung)
        $code | Should -Be 2
    }

    It 'install outside a game folder exits 6 (<HostName>)' -TestCases $Hosts {
        $mod = New-ModFolder -Root $script:Root -Name ('proc nogame ' + $HostName) -NoGame
        $code = Invoke-SetupProcess -Exe $Exe -Script (Join-Path $mod 'Braindance3D-Setup.ps1') -Arguments @('-Action', 'install', '-NoPause', '-RegistryPath', 'HKCU:\SOFTWARE\Braindance3D-Tests\x', '-SamsungDefaultDir', $script:NoSamsung)
        $code | Should -Be 6
    }

    It 'install from inside %TEMP% exits 5 (<HostName>)' -TestCases $Hosts -Skip:(-not $IsWin) {
        $mod = New-ModFolder -Root $script:Root -Name ('proc temp ' + $HostName)
        $saved = $env:TEMP
        try {
            $env:TEMP = $script:Root
            $code = Invoke-SetupProcess -Exe $Exe -Script (Join-Path $mod 'Braindance3D-Setup.ps1') -Arguments @('-Action', 'install', '-NoPause', '-RegistryPath', $script:Key, '-SamsungDefaultDir', $script:NoSamsung)
        } finally {
            $env:TEMP = $saved
        }
        $code | Should -Be 5
    }

    It 'an unknown action fails (<HostName>)' -TestCases $Hosts {
        $mod = New-ModFolder -Root $script:Root -Name ('proc bad ' + $HostName)
        $code = Invoke-SetupProcess -Exe $Exe -Script (Join-Path $mod 'Braindance3D-Setup.ps1') -Arguments @('-Action', 'reinstall', '-NoPause', '-RegistryPath', 'HKCU:\SOFTWARE\Braindance3D-Tests\x')
        $code | Should -Not -Be 0
    }

    It 'install, status and uninstall round trip, defaulting to the script''s folder (<HostName>)' -TestCases $Hosts -Skip:(-not $IsWin) {
        $mod = New-ModFolder -Root $script:Root -Name ('proc [round trip] ' + $script:NonAscii + ' ' + $HostName)
        $ps1 = Join-Path $mod 'Braindance3D-Setup.ps1'
        $out = Join-Path $script:Root ('out-' + $HostName + '.txt')
        Invoke-SetupProcess -Exe $Exe -Script $ps1 -Arguments @('-Action', 'install', '-NoPause', '-RegistryPath', $script:Key, '-SamsungDefaultDir', $script:NoSamsung) -OutFile $out | Should -Be 0
        Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $mod
        Invoke-SetupProcess -Exe $Exe -Script $ps1 -Arguments @('-Action', 'status', '-NoPause', '-RegistryPath', $script:Key) | Should -Be 0
        Invoke-SetupProcess -Exe $Exe -Script $ps1 -Arguments @('-Action', 'uninstall', '-NoPause', '-RegistryPath', $script:Key) | Should -Be 0
        Test-Path -LiteralPath $script:Key | Should -BeFalse
    }

    It 'refusing a foreign SDK exits 3, -Force exits 0 (<HostName>)' -TestCases $Hosts -Skip:(-not $IsWin) {
        $mod = New-ModFolder -Root $script:Root -Name ('proc foreign ' + $HostName)
        $hub = New-ForeignSdk -Root $script:Root -Name ('Hub ' + $HostName)
        Set-TestValues $script:Key @{ InstallPath = $hub }
        $ps1 = Join-Path $mod 'Braindance3D-Setup.ps1'
        Invoke-SetupProcess -Exe $Exe -Script $ps1 -Arguments @('-Action', 'install', '-NoPause', '-RegistryPath', $script:Key, '-SamsungDefaultDir', $script:NoSamsung) | Should -Be 3
        Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $hub
        Invoke-SetupProcess -Exe $Exe -Script $ps1 -Arguments @('-Action', 'install', '-Force', '-NoPause', '-RegistryPath', $script:Key, '-SamsungDefaultDir', $script:NoSamsung) | Should -Be 0
        Invoke-SetupProcess -Exe $Exe -Script $ps1 -Arguments @('-Action', 'uninstall', '-NoPause', '-RegistryPath', $script:Key) | Should -Be 0
        Get-TestValues $script:Key | Should -BeExactly "InstallPath=$hub"
    }

    It '-AnyFolder lets a staging folder install (<HostName>)' -TestCases $Hosts -Skip:(-not $IsWin) {
        $mod = New-ModFolder -Root $script:Root -Name ('proc staging ' + $HostName) -NoGame
        $ps1 = Join-Path $mod 'Braindance3D-Setup.ps1'
        Invoke-SetupProcess -Exe $Exe -Script $ps1 -Arguments @('-Action', 'install', '-AnyFolder', '-NoPause', '-RegistryPath', $script:Key, '-SamsungDefaultDir', $script:NoSamsung) | Should -Be 0
        Get-TestValue $script:Key 'InstallPath' | Should -BeExactly $mod
    }
}

Describe 'Shipped files agree with each other' {
    It 'the script parses without errors' {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($script:SetupScript, [ref]$tokens, [ref]$errors) | Out-Null
        @($errors).Count | Should -Be 0
    }
    It 'the script is plain ASCII (Windows PowerShell 5.1 reads BOM-less files as ANSI)' {
        $bytes = [System.IO.File]::ReadAllBytes($script:SetupScript)
        @($bytes | Where-Object { $_ -gt 127 }).Count | Should -Be 0
    }
    It 'the script''s version matches src/version.h' {
        $script:Version | Should -BeExactly (Get-VersionH)
    }
    It 'the script''s product name matches the DLL version resource (src/version.rc)' {
        $script:ProductName | Should -BeExactly (Get-RcProductName)
    }
    It 'the built DLL carries that product name and version' -Skip:(-not $IsWin) {
        if (-not $script:BuiltDll) { Set-ItResult -Skipped -Because 'no built DLL (run ./build.sh)'; return }
        $vi = (Get-Item -LiteralPath $script:BuiltDll).VersionInfo
        $vi.ProductName | Should -BeExactly $script:ProductName
        $vi.ProductVersion | Should -BeExactly $script:Version
    }
    It 'fomod/info.xml and CHANGELOG.md name the same version' {
        $v = Get-VersionH
        $info = Join-Path $script:RepoRoot 'package\fomod\info.xml'
        if (Test-Path -LiteralPath $info) { ([xml](Get-Content -LiteralPath $info -Raw)).fomod.Version | Should -BeExactly $v }
        Get-Content -LiteralPath (Join-Path $script:RepoRoot 'CHANGELOG.md') -Raw | Should -Match ('(?m)^## ' + [regex]::Escape($v) + '\b')
    }
    It 'the settings file the script looks for ships next to it, with the documented defaults' {
        $ini = Join-Path $script:ModSource $script:SettingsFile
        Test-Path -LiteralPath $ini | Should -BeTrue
        $text = Get-Content -LiteralPath $ini -Raw
        $text | Should -Match '(?m)^\[Log\]'
        $text | Should -Match '(?m)^Level\s*=\s*1\s*$'
        $text | Should -Match '(?m)^\[Display\]'
        $text | Should -Match '(?m)^Monitor\s*=\s*auto\s*$'
    }
    It 'the engine ini turns on the side-by-side options' {
        $text = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'package\engine\config\platform\pc\braindance3d.ini') -Raw
        $text | Should -Match '(?m)^\[Odyssey3D\]'
        foreach ($opt in 'OverrideEnabled', 'StereoEnabled', 'SBSDebug') { $text | Should -Match ('(?m)^' + $opt + '\s*=\s*true\s*$') }
    }
    It 'the RED4ext auto-setup plugin uses the same key, value names and files as the script' {
        # src/braindance3d_autosetup.c registers the mod when the game starts; this script must be able to
        # show and undo what it wrote, so both sides have to agree on every name.
        $c = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'src\braindance3d_autosetup.c') -Raw
        $ps = Get-Content -LiteralPath $script:SetupScript -Raw
        $defines = @{}
        foreach ($m in [regex]::Matches($c, '(?m)^#define (\w+) L"((?:[^"\\]|\\.)*)"')) {
            $defines[$m.Groups[1].Value] = $m.Groups[2].Value -replace '\\\\', '\'
        }
        $ps -match "\[string\]\`$RegistryPath = '([^']*)'" | Should -BeTrue
        ('HKCU:\' + $defines['DEFAULT_KEY']) | Should -BeExactly $Matches[1]
        $defines['VALUE_INSTALL_PATH'] | Should -BeExactly 'InstallPath'
        $defines['VALUE_MARKER'] | Should -BeExactly $script:MarkerName
        $defines['VALUE_OWN_PATH'] | Should -BeExactly $script:OwnPathName
        $defines['VALUE_PREVIOUS'] | Should -BeExactly $script:PreviousName
        $defines['NO_PREVIOUS'] | Should -BeExactly $script:NoPreviousValue
        $defines['SDK_RELATIVE'] | Should -BeExactly ('\' + $script:SdkRelativePath)
        $defines['SETTINGS_FILE'] | Should -BeExactly $script:SettingsFile
        $defines['PRODUCT_NAME'] | Should -BeExactly $script:ProductName
        # Samsung's default folder, which both refuse to override without -Force.
        $c | Should -Match ([regex]::Escape('L"%ls\\Odyssey 3D Hub"'))
        $ps | Should -Match ([regex]::Escape("Join-Path `$env:ProgramFiles 'Odyssey 3D Hub'"))
    }
    It 'the built RED4ext plugin carries the product name and version' -Skip:(-not $IsWin) {
        $plugin = Join-Path $script:RepoRoot 'build\Braindance3D.dll'
        if (-not (Test-Path -LiteralPath $plugin)) { Set-ItResult -Skipped -Because 'no built plugin (run ./build.sh)'; return }
        $vi = (Get-Item -LiteralPath $plugin).VersionInfo
        $vi.ProductName | Should -BeExactly $script:ProductName
        $vi.ProductVersion | Should -BeExactly $script:Version
        $vi.OriginalFilename | Should -BeExactly 'Braindance3D.dll'
    }
    It 'the FOMOD installs the game folders the zip contains' {
        $xml = [xml](Get-Content -LiteralPath (Join-Path $script:RepoRoot 'package\fomod\ModuleConfig.xml') -Raw)
        $folders = @($xml.config.requiredInstallFiles.folder | ForEach-Object { $_.source + '>' + $_.destination })
        ($folders -join ',') | Should -BeExactly 'bin>bin,engine>engine,red4ext>red4ext'
    }
    It '<Bat> runs the script with -Action <Action> -NoPause and passes extra arguments on' -TestCases @(
        @{ Bat = 'Install-Braindance3D.bat'; Action = 'install' }
        @{ Bat = 'Uninstall-Braindance3D.bat'; Action = 'uninstall' }
    ) {
        $text = Get-Content -LiteralPath (Join-Path $script:ModSource $Bat) -Raw
        $text | Should -Match 'WindowsPowerShell\\v1\.0\\powershell\.exe'
        $text | Should -Match '-ExecutionPolicy Bypass'
        $text | Should -Match ('-File "%~dp0Braindance3D-Setup\.ps1" -Action ' + $Action + ' -NoPause %\*')
        $text | Should -Match '(?m)^pause'
    }
}

Describe 'Test coverage of the script' {
    It 'every function in Braindance3D-Setup.ps1 is tested (update this list when adding one)' {
        $tested = @('Initialize-Log', 'Write-Log', 'Get-NormalizedPath', 'Test-SamePath', 'Test-PathUnder', 'Get-ParentPath',
            'Get-RegistryValue', 'Get-SdkPath', 'Get-SdkInfo', 'Get-RegistryState', 'Test-InstallPathIsOurs', 'Get-GameInfo',
            'Test-MinimumVersion', 'Get-SamsungDefaultDir', 'Test-IsElevated', 'Remove-EmptyKey', 'Remove-Bookkeeping',
            'Show-Status', 'Invoke-Install', 'Invoke-Uninstall', 'Invoke-Main')
        $ast = [System.Management.Automation.Language.Parser]::ParseFile($script:SetupScript, [ref]$null, [ref]$null)
        $defined = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) | ForEach-Object { $_.Name })
        $untested = @($defined | Where-Object { $tested -notcontains $_ })
        ($untested -join ', ') | Should -BeExactly ''
    }
}
