#Requires -Version 5.1

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $PackageRoot
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PackageRoot)
$pluginRoot = Join-Path $root 'bambu-printer-overlay'
$dllPath = Join-Path $pluginRoot 'bin\64bit\bambu-printer-overlay.dll'
$localePath = Join-Path $pluginRoot 'data\locale\en-US.ini'
$requiredFiles = @(
    $dllPath,
    $localePath,
    (Join-Path $pluginRoot 'data\README.md'),
    (Join-Path $pluginRoot 'data\licenses\LICENSE'),
    (Join-Path $pluginRoot 'data\licenses\GPL-2.0-or-later.txt'),
    (Join-Path $pluginRoot 'data\licenses\THIRD_PARTY_NOTICES.md')
)

foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required package file is missing: $requiredFile"
    }
}

$stream = [IO.File]::OpenRead($dllPath)
try {
    $reader = [IO.BinaryReader]::new($stream)
    $stream.Position = 0x3c
    $peOffset = $reader.ReadInt32()
    $stream.Position = $peOffset
    $signature = $reader.ReadUInt32()
    $machine = $reader.ReadUInt16()
} finally {
    if ($reader) { $reader.Dispose() } else { $stream.Dispose() }
}
if ($signature -ne 0x00004550) {
    throw 'Plugin DLL does not contain a valid PE signature.'
}
if ($machine -ne 0x8664) {
    throw ('Plugin DLL is not x64 (machine 0x{0:X4}).' -f $machine)
}

$forbiddenExtensions = @('.py', '.pyc', '.pyo', '.pyd')
$forbiddenFiles = Get-ChildItem -LiteralPath $pluginRoot -Recurse -File | Where-Object {
    $forbiddenExtensions -contains $_.Extension.ToLowerInvariant()
}
if ($forbiddenFiles) {
    throw "Python runtime files were found in the native package: $($forbiddenFiles.FullName -join ', ')"
}

$textFiles = Get-ChildItem -LiteralPath $root -Recurse -File -Include '*.md', '*.ini', '*.json', '*.ps1', '*.txt'
$developerPathMatches = $textFiles | Select-String -Pattern 'C:\\Users\\[^\\]+\\vscode|/Users/[^/]+/' -ErrorAction SilentlyContinue
if ($developerPathMatches) {
    throw "Developer-specific paths were found in the package: $($developerPathMatches.Path -join ', ')"
}
$secretMatches = $textFiles | Select-String -Pattern '(?i)(ACCESS_CODE|BAMBU_CLOUD_TOKEN|PASSWORD)\s*=\s*[^\s\{]' -ErrorAction SilentlyContinue
if ($secretMatches) {
    throw "Possible credential assignment was found in the package: $($secretMatches.Path -join ', ')"
}

Write-Output "Validated native OBS package at $root"
Write-Output "DLL: $dllPath"
