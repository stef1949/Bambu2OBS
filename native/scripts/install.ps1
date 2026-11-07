#Requires -Version 5.1

[CmdletBinding()]
param(
    [string] $ObsPluginRoot = "$env:ProgramData\obs-studio\plugins",
    [switch] $AllowObsRunningForIsolatedTest
)

$ErrorActionPreference = 'Stop'
$packageRoot = $PSScriptRoot
$sourcePlugin = Join-Path $packageRoot 'bambu-printer-overlay'
$manifestPath = Join-Path $packageRoot 'manifest.json'

if (-not $AllowObsRunningForIsolatedTest -and (Get-Process obs64 -ErrorAction SilentlyContinue)) {
    throw 'Close OBS Studio before installing Bambu Printer Overlay.'
}
if (-not (Test-Path -LiteralPath $sourcePlugin -PathType Container)) {
    throw "Package payload is missing: $sourcePlugin"
}
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Package manifest is missing: $manifestPath"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
foreach ($entry in $manifest.files) {
    $candidate = Join-Path $packageRoot $entry.path
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "Package file is missing: $($entry.path)"
    }
    $actualHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
    if ($actualHash -ne $entry.sha256) {
        throw "Package hash mismatch: $($entry.path)"
    }
}

$resolvedRoot = [IO.Path]::GetFullPath($ObsPluginRoot)
if ([string]::IsNullOrWhiteSpace($resolvedRoot) -or $resolvedRoot.Length -lt 4) {
    throw 'The OBS plugin root is invalid.'
}
$programDataRoot = [IO.Path]::GetFullPath($env:ProgramData).TrimEnd('\') + '\'

# OBS before 33 does not look for this layout, so the plugin would silently not load.
if ($resolvedRoot.StartsWith($programDataRoot, [StringComparison]::OrdinalIgnoreCase)) {
    $obsInstall = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\OBS Studio' -ErrorAction SilentlyContinue).'(default)'
    if ($obsInstall) {
        $obsExecutable = Join-Path $obsInstall 'bin\64bit\obs64.exe'
        if (Test-Path -LiteralPath $obsExecutable -PathType Leaf) {
            $obsMajor = (Get-Item -LiteralPath $obsExecutable).VersionInfo.FileMajorPart
            if ($obsMajor -lt 33) {
                throw "Bambu Printer Overlay needs OBS Studio 33 or newer. Update OBS first (found major version $obsMajor)."
            }
        }
    }
}

$currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($currentIdentity)
if (
    $resolvedRoot.StartsWith($programDataRoot, [StringComparison]::OrdinalIgnoreCase) -and
    -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
) {
    throw 'Installing under ProgramData requires an elevated PowerShell window.'
}
$destination = Join-Path $resolvedRoot 'bambu-printer-overlay'
New-Item -ItemType Directory -Path $resolvedRoot -Force | Out-Null
$backupPath = $null

if (Test-Path -LiteralPath $destination) {
    $backupName = 'bambu-printer-overlay.backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
    $backupPath = Join-Path $resolvedRoot $backupName
    Move-Item -LiteralPath $destination -Destination $backupPath
    Write-Output "Existing plugin moved to $backupPath"
}

try {
    Copy-Item -LiteralPath $sourcePlugin -Destination $destination -Recurse
    Copy-Item -LiteralPath (Join-Path $packageRoot 'uninstall.ps1') -Destination (Join-Path $destination 'data\uninstall.ps1')
} catch {
    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($backupPath -and (Test-Path -LiteralPath $backupPath)) {
        Move-Item -LiteralPath $backupPath -Destination $destination
    }
    throw
}

if ($resolvedRoot.StartsWith($programDataRoot, [StringComparison]::OrdinalIgnoreCase)) {
    $uninstallKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\BambuPrinterOverlay'
    $uninstallScript = Join-Path $destination 'data\uninstall.ps1'
    New-Item -Path $uninstallKey -Force | Out-Null
    Set-ItemProperty -Path $uninstallKey -Name DisplayName -Value 'Bambu Printer Overlay'
    Set-ItemProperty -Path $uninstallKey -Name DisplayVersion -Value ([string]$manifest.version)
    Set-ItemProperty -Path $uninstallKey -Name Publisher -Value 'Bambu2OBS contributors'
    Set-ItemProperty -Path $uninstallKey -Name InstallLocation -Value $destination
    Set-ItemProperty -Path $uninstallKey -Name UninstallString -Value ('powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $uninstallScript)
    Set-ItemProperty -Path $uninstallKey -Name NoModify -Type DWord -Value 1
    Set-ItemProperty -Path $uninstallKey -Name NoRepair -Type DWord -Value 1
}
Write-Output "Installed Bambu Printer Overlay to $destination"
Write-Output 'Start OBS Studio, add a source, and select Bambu Printer Overlay.'
