# An in-memory stand-in for the HKCU: registry drive, for running the registry tests on Linux or macOS
# (set BD3D_FAKE_REGISTRY=1). It shadows the item cmdlets the setup script and the tests use with
# functions that handle "HKCU:\..." paths themselves and pass every other path to the real cmdlets.
# It models only what is needed here: keys, string/DWORD values, ValueCount/SubKeyCount. On Windows the
# real registry is always used. It is a development aid: CI on Windows is what counts.

$script:FakeReg = @{}

function Test-FakeRegPath {
    param([string]$Path)
    return ($Path -match '^(?i)HKCU:[\\/]')
}

function ConvertTo-FakeRegKey {
    param([string]$Path)
    return ($Path -replace '/', '\').TrimEnd('\').ToLowerInvariant()
}

function Get-FakeRegChildren {
    param([string]$Norm)
    $prefix = $Norm + '\'
    return @($script:FakeReg.Keys | Where-Object { $_.StartsWith($prefix) -and $_.Substring($prefix.Length) -notmatch '\\' })
}

function New-FakeRegItem {
    param([string]$Norm)
    $entry = $script:FakeReg[$Norm]
    $o = [pscustomobject]@{
        Name        = $entry.Name
        PSPath      = $entry.Name
        ValueCount  = $entry.Values.Count
        SubKeyCount = @(Get-FakeRegChildren $Norm).Count
        Values      = $entry.Values
    }
    $o | Add-Member -MemberType ScriptMethod -Name GetValue -Value { param($n) if ($this.Values.Contains($n)) { $this.Values[$n].Value } else { $null } }
    $o | Add-Member -MemberType ScriptMethod -Name GetValueNames -Value { [string[]]@($this.Values.Keys) }
    $o | Add-Member -MemberType ScriptMethod -Name GetValueKind -Value { param($n) $this.Values[$n].Kind }
    return $o
}

function Get-FakeRegEntry {
    param([string]$Path)
    $norm = ConvertTo-FakeRegKey $Path
    if (-not $script:FakeReg.ContainsKey($norm)) { throw "Cannot find path '$Path' because it does not exist." }
    return $script:FakeReg[$norm]
}

function Test-Path {
    [CmdletBinding()]
    param([Parameter(Position = 0)][string[]]$Path, [string[]]$LiteralPath, [string]$PathType, [switch]$IsValid)
    $p = @($LiteralPath + $Path | Where-Object { $_ })[0]
    if ($p -and (Test-FakeRegPath $p)) { return $script:FakeReg.ContainsKey((ConvertTo-FakeRegKey $p)) }
    Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
}

function New-Item {
    [CmdletBinding()]
    param([Parameter(Position = 0)][string[]]$Path, [string]$ItemType, [switch]$Force, $Value, [string]$Name)
    $p = @($Path)[0]
    if ($p -and (Test-FakeRegPath $p)) {
        $norm = ConvertTo-FakeRegKey $p
        $parts = ($p -replace '/', '\').TrimEnd('\').Split('\')
        for ($i = 2; $i -le $parts.Count; $i++) {
            $sub = ($parts[0..($i - 1)] -join '\')
            $n = ConvertTo-FakeRegKey $sub
            if (-not $script:FakeReg.ContainsKey($n)) {
                if ($i -lt $parts.Count -and -not $Force -and $i -gt 2) { throw "Parent key '$sub' does not exist" }
                $script:FakeReg[$n] = @{ Name = $sub; Values = [ordered]@{} }
            }
        }
        return (New-FakeRegItem $norm)
    }
    Microsoft.PowerShell.Management\New-Item @PSBoundParameters
}

function Get-Item {
    [CmdletBinding()]
    param([Parameter(Position = 0)][string[]]$Path, [string[]]$LiteralPath, [switch]$Force)
    $p = @($LiteralPath + $Path | Where-Object { $_ })[0]
    if ($p -and (Test-FakeRegPath $p)) {
        Get-FakeRegEntry $p | Out-Null
        return (New-FakeRegItem (ConvertTo-FakeRegKey $p))
    }
    Microsoft.PowerShell.Management\Get-Item @PSBoundParameters
}

function Remove-Item {
    [CmdletBinding()]
    param([Parameter(Position = 0)][string[]]$Path, [string[]]$LiteralPath, [switch]$Recurse, [switch]$Force)
    $p = @($LiteralPath + $Path | Where-Object { $_ })[0]
    if ($p -and (Test-FakeRegPath $p)) {
        Get-FakeRegEntry $p | Out-Null
        $norm = ConvertTo-FakeRegKey $p
        $children = @($script:FakeReg.Keys | Where-Object { $_.StartsWith($norm + '\') })
        if ($children.Count -and -not $Recurse) { throw "Key '$p' has subkeys and -Recurse was not given" }
        foreach ($c in $children) { $script:FakeReg.Remove($c) }
        $script:FakeReg.Remove($norm)
        return
    }
    Microsoft.PowerShell.Management\Remove-Item @PSBoundParameters
}

function Get-ItemProperty {
    [CmdletBinding()]
    param([Parameter(Position = 0)][string[]]$Path, [string[]]$LiteralPath, [Parameter(Position = 1)][string[]]$Name)
    $p = @($LiteralPath + $Path | Where-Object { $_ })[0]
    if ($p -and (Test-FakeRegPath $p)) {
        $entry = Get-FakeRegEntry $p
        $props = [ordered]@{ PSPath = $entry.Name; PSChildName = ($entry.Name.Split('\')[-1]) }
        foreach ($k in $entry.Values.Keys) { $props[$k] = $entry.Values[$k].Value }
        return [pscustomobject]$props
    }
    Microsoft.PowerShell.Management\Get-ItemProperty @PSBoundParameters
}

function Set-ItemProperty {
    [CmdletBinding()]
    param([Parameter(Position = 0)][string[]]$Path, [string[]]$LiteralPath, [Parameter(Position = 1)][string]$Name, [Parameter(Position = 2)]$Value)
    $p = @($LiteralPath + $Path | Where-Object { $_ })[0]
    if ($p -and (Test-FakeRegPath $p)) {
        $entry = Get-FakeRegEntry $p
        $entry.Values[$Name] = @{ Value = [string]$Value; Kind = 'String' }
        return
    }
    Microsoft.PowerShell.Management\Set-ItemProperty @PSBoundParameters
}

function New-ItemProperty {
    [CmdletBinding()]
    param([Parameter(Position = 0)][string[]]$Path, [string[]]$LiteralPath, [string]$Name, $Value, [string]$PropertyType = 'String', [switch]$Force)
    $p = @($LiteralPath + $Path | Where-Object { $_ })[0]
    if ($p -and (Test-FakeRegPath $p)) {
        $entry = Get-FakeRegEntry $p
        if ($entry.Values.Contains($Name) -and -not $Force) { throw "Value '$Name' exists" }
        $v = if ($PropertyType -eq 'DWord') { [int]$Value } else { [string]$Value }
        $entry.Values[$Name] = @{ Value = $v; Kind = $PropertyType }
        return [pscustomobject]@{ $Name = $v }
    }
    Microsoft.PowerShell.Management\New-ItemProperty @PSBoundParameters
}

function Remove-ItemProperty {
    [CmdletBinding()]
    param([Parameter(Position = 0)][string[]]$Path, [string[]]$LiteralPath, [Parameter(Position = 1)][string[]]$Name)
    $p = @($LiteralPath + $Path | Where-Object { $_ })[0]
    if ($p -and (Test-FakeRegPath $p)) {
        $entry = Get-FakeRegEntry $p
        foreach ($n in $Name) {
            if ($entry.Values.Contains($n)) { $entry.Values.Remove($n) }
            else { Write-Error "Property $n does not exist at path $p." }
        }
        return
    }
    Microsoft.PowerShell.Management\Remove-ItemProperty @PSBoundParameters
}
