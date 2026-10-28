<#
.SYNOPSIS
    Runs the Pester 5 tests of Braindance3D-Setup.ps1 (tests/powershell/*.Tests.ps1).

.DESCRIPTION
    Works in Windows PowerShell 5.1 and PowerShell 7. Registry tests run on Windows only and use throwaway
    keys under HKCU:\SOFTWARE\Braindance3D-Tests; elsewhere they are skipped.
    Needs Pester 5.3 or newer. With -InstallPester, installs it for the current user when it is missing.

.PARAMETER CI
    For CI: installs Pester when missing (like -InstallPester). The exit code is 1 when a test failed either way.

.PARAMETER IncludeKnownBugs
    Also run the tests tagged KnownBug (bugs that are reported but not fixed yet). Same as BD3D_RUN_KNOWN_BUGS=1.

.PARAMETER OutputFile
    Write NUnit XML results to this file.

.PARAMETER Path
    Test files or folders to run (default: tests/powershell).

.EXAMPLE
    pwsh tests/Run-PesterTests.ps1
    powershell -ExecutionPolicy Bypass -File tests\Run-PesterTests.ps1 -CI -OutputFile build\tests\pester.xml
#>
[CmdletBinding()]
param(
    [switch]$CI,
    [switch]$InstallPester,
    [switch]$IncludeKnownBugs,
    [string]$OutputFile = '',
    [string[]]$Path = @(),
    [ValidateSet('None', 'Normal', 'Detailed', 'Diagnostic')]
    [string]$Output = 'Detailed'
)

$ErrorActionPreference = 'Stop'
$minimum = [version]'5.3.0'

function Get-Pester5 {
    return Get-Module -ListAvailable -Name Pester | Where-Object { $_.Version -ge $minimum } | Sort-Object Version -Descending | Select-Object -First 1
}

$pester = Get-Pester5
if (-not $pester -and ($InstallPester -or $CI)) {
    Write-Host "Installing Pester $minimum or newer for the current user..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    if ($PSVersionTable.PSVersion.Major -lt 6) {
        Install-PackageProvider -Name NuGet -MinimumVersion 2.8.5.201 -Scope CurrentUser -Force | Out-Null
    }
    Install-Module -Name Pester -MinimumVersion $minimum -Scope CurrentUser -Force -SkipPublisherCheck -AllowClobber
    $pester = Get-Pester5
}
if (-not $pester) {
    Write-Error "Pester $minimum or newer is needed. Install it with: Install-Module Pester -MinimumVersion $minimum -Scope CurrentUser -Force -SkipPublisherCheck (or run this script with -InstallPester)."
    exit 2
}
Remove-Module Pester -ErrorAction SilentlyContinue
Import-Module $pester.Path -Force

if (-not $Path -or $Path.Count -eq 0) { $Path = @(Join-Path $PSScriptRoot 'powershell') }
$knownBugs = $IncludeKnownBugs -or ($env:BD3D_RUN_KNOWN_BUGS -and $env:BD3D_RUN_KNOWN_BUGS -ne '0')

$config = New-PesterConfiguration
$config.Run.Path = $Path
$config.Run.PassThru = $true
$config.Output.Verbosity = $Output
if (-not $knownBugs) { $config.Filter.ExcludeTag = @('KnownBug') }
if ($OutputFile) {
    $dir = Split-Path -Parent $OutputFile
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $config.TestResult.Enabled = $true
    $config.TestResult.OutputFormat = 'NUnitXml'
    $config.TestResult.OutputPath = $OutputFile
}

Write-Host ("Pester {0} on PowerShell {1} ({2}){3}" -f $pester.Version, $PSVersionTable.PSVersion,
    [System.Environment]::OSVersion.Platform, $(if ($knownBugs) { ', including KnownBug tests' } else { ', KnownBug tests excluded' }))
$result = Invoke-Pester -Configuration $config
Write-Host ("Passed {0}, failed {1}, skipped {2}, not run {3}" -f $result.PassedCount, $result.FailedCount, $result.SkippedCount, $result.NotRunCount)
if ($result.FailedCount -gt 0 -or $result.Result -eq 'Failed') {
    exit 1
}
exit 0
