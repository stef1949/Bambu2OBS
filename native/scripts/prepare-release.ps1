#Requires -Version 5.1

<#
.SYNOPSIS
Checks that a tag can be released and, given built artifacts, writes its notes.

.DESCRIPTION
With only -Tag it checks the tag, the version, and the changelog, so a bad
release fails before anything is built. With -ReleaseRoot and -NotesPath it also
checks the artifacts against their checksums and writes the release notes.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Tag,
    [string] $ReleaseRoot = '',
    [string] $NotesPath = ''
)

$ErrorActionPreference = 'Stop'
$nativeRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $nativeRoot '..'))

if ([bool]$ReleaseRoot -ne [bool]$NotesPath) {
    throw '-ReleaseRoot and -NotesPath must be used together.'
}

if ($Tag -cnotmatch '\Av(?<version>[0-9]+\.[0-9]+\.[0-9]+)(?<suffix>-[0-9A-Za-z.-]+)?\z') {
    throw "Tag '$Tag' is not a release tag. Use v1.2.3, or v1.2.3-rc.1 for a prerelease."
}
$version = $Matches['version']
$prerelease = [bool]$Matches['suffix']

# The package takes its name from buildspec.json and the DLL its version from CMake.
$buildSpecVersion = [string](Get-Content -LiteralPath (Join-Path $nativeRoot 'buildspec.json') -Raw | ConvertFrom-Json).version
$cmakeLists = Get-Content -LiteralPath (Join-Path $nativeRoot 'CMakeLists.txt') -Raw
if ($cmakeLists -notmatch '(?m)^project\(bambu-printer-overlay VERSION (?<version>[0-9.]+)') {
    throw 'Could not find the project version in native/CMakeLists.txt.'
}
$cmakeVersion = $Matches['version']
if ($buildSpecVersion -ne $version -or $cmakeVersion -ne $version) {
    throw "Tag $Tag does not match the source: native/buildspec.json is $buildSpecVersion and native/CMakeLists.txt is $cmakeVersion."
}

$changes = [Collections.Generic.List[string]]::new()
$inSection = $false
foreach ($line in (Get-Content -LiteralPath (Join-Path $repoRoot 'CHANGELOG.md') -Encoding UTF8)) {
    if ($line -match '^## ') {
        if ($inSection) { break }
        $inSection = $line -match ('^## \[?' + [regex]::Escape($version) + '\]?(\s|$)')
        continue
    }
    if ($inSection) { $changes.Add($line) }
}
$changelog = ($changes -join "`n").Trim()
if (-not $changelog) {
    throw "CHANGELOG.md has no entries under a '## $version' heading. Move the Unreleased entries there before tagging."
}

if ($env:GITHUB_OUTPUT) {
    # Lowercase, because workflow expressions compare these as text.
    "version=$version", "prerelease=$($prerelease.ToString().ToLowerInvariant())" |
        Add-Content -LiteralPath $env:GITHUB_OUTPUT -Encoding UTF8
}
Write-Output "Tag $Tag is version $version (prerelease: $prerelease)"

if (-not $ReleaseRoot) {
    return
}

$artifactBase = "Bambu-Printer-Overlay-$version-Windows-x64"
$artifacts = @("$artifactBase-Setup.exe", "$artifactBase.zip")
$expected = $artifacts + ($artifacts | ForEach-Object { "$_.sha256" })

$present = @(Get-ChildItem -LiteralPath $ReleaseRoot -File | ForEach-Object { $_.Name })
$missing = @($expected | Where-Object { $_ -notin $present })
if ($missing) { throw "Missing release files: $($missing -join ', ')" }
$unexpected = @($present | Where-Object { $_ -notin $expected })
if ($unexpected) { throw "Unexpected release files: $($unexpected -join ', ')" }

$rows = foreach ($artifact in $artifacts) {
    $hash = (Get-FileHash -LiteralPath (Join-Path $ReleaseRoot $artifact) -Algorithm SHA256).Hash.ToLowerInvariant()
    # Exact, because sha256sum -c rejects a checksum file with a carriage return in it.
    $sidecar = Get-Content -LiteralPath (Join-Path $ReleaseRoot "$artifact.sha256") -Raw
    if ($sidecar -cne "$hash  $artifact`n") {
        throw "$artifact does not match its checksum file."
    }
    "| ``$artifact`` | ``$hash`` |"
}

$notes = @"
$changelog

## Install

You need OBS Studio 33 or newer on 64-bit Windows.

1. Close OBS.
2. Run ``$artifactBase-Setup.exe`` and approve the administrator prompt.
3. Start OBS, then add **Bambu Printer Overlay** as a source.

To install without the wizard, or into a portable OBS, use the ZIP and follow the README inside it.

## Checksums

The installer isn't code-signed, so Microsoft Defender SmartScreen may warn about it. Check the SHA-256 before running it:

| File | SHA-256 |
| --- | --- |
$($rows -join "`n")

``````powershell
(Get-FileHash .\$artifactBase-Setup.exe -Algorithm SHA256).Hash
``````

Each file also has a ``.sha256`` file beside it, for ``sha256sum -c``.
"@

# Here-strings take this file's line endings, and the notes should not depend on them.
[IO.File]::WriteAllText([IO.Path]::GetFullPath($NotesPath), ($notes -replace "`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
Write-Output "Wrote release notes to $NotesPath"
