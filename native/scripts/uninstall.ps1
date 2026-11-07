#Requires -Version 5.1

[CmdletBinding(SupportsShouldProcess)]
param(
    [string] $ObsPluginRoot = "$env:ProgramData\obs-studio\plugins",
    [switch] $AllowObsRunningForIsolatedTest
)

$ErrorActionPreference = 'Stop'
if (-not $AllowObsRunningForIsolatedTest -and (Get-Process obs64 -ErrorAction SilentlyContinue)) {
    throw 'Close OBS Studio before uninstalling Bambu Printer Overlay.'
}

$resolvedRoot = [IO.Path]::GetFullPath($ObsPluginRoot)
$programDataRoot = [IO.Path]::GetFullPath($env:ProgramData).TrimEnd('\') + '\'
$currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($currentIdentity)
if (
    $resolvedRoot.StartsWith($programDataRoot, [StringComparison]::OrdinalIgnoreCase) -and
    -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
) {
    throw 'Removing a plugin under ProgramData requires an elevated PowerShell window.'
}
$destination = [IO.Path]::GetFullPath((Join-Path $resolvedRoot 'bambu-printer-overlay'))
$expectedParent = [IO.Path]::GetFullPath($resolvedRoot).TrimEnd('\') + '\'
if (-not $destination.StartsWith($expectedParent, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to remove a path outside the selected OBS plugin root.'
}

if (-not (Test-Path -LiteralPath $destination)) {
    Write-Output 'Bambu Printer Overlay is not installed at the selected path.'
    exit 0
}
if ($PSCmdlet.ShouldProcess($destination, 'Remove Bambu Printer Overlay')) {
    Remove-Item -LiteralPath $destination -Recurse -Force
    $uninstallKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\BambuPrinterOverlay'
    if (
        $resolvedRoot.StartsWith($programDataRoot, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $uninstallKey)
    ) {
        Remove-Item -LiteralPath $uninstallKey -Recurse -Force
    }
    Write-Output "Removed $destination"
}
