#Requires -Version 5.1

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string] $Configuration = 'Release',
    [string] $CMake = 'cmake',
    [string] $InnoCompiler = ''
)

$ErrorActionPreference = 'Stop'
$nativeRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $nativeRoot '..'))
$buildRoot = Join-Path $nativeRoot 'build_x64'
$buildSpec = Get-Content -LiteralPath (Join-Path $nativeRoot 'buildspec.json') -Raw | ConvertFrom-Json
$version = [string]$buildSpec.version
$artifactBase = "Bambu-Printer-Overlay-$version-Windows-x64"
$releaseRoot = Join-Path $repoRoot 'release'
$stagingRoot = Join-Path $nativeRoot "package-$Configuration"

function Assert-ChildPath([string] $Candidate, [string] $Parent) {
    $candidatePath = [IO.Path]::GetFullPath($Candidate)
    $parentPath = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not $candidatePath.StartsWith($parentPath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify path outside $Parent`: $Candidate"
    }
}

Assert-ChildPath $stagingRoot $nativeRoot
Assert-ChildPath $releaseRoot $repoRoot
if (Test-Path -LiteralPath $stagingRoot) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingRoot, $releaseRoot -Force | Out-Null

& $CMake --install $buildRoot --config $Configuration --prefix $stagingRoot
if ($LASTEXITCODE -ne 0) { throw 'CMake install failed.' }

Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install.ps1') -Destination $stagingRoot
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'uninstall.ps1') -Destination $stagingRoot
Copy-Item -LiteralPath (Join-Path $nativeRoot 'PACKAGE_README.md') -Destination (Join-Path $stagingRoot 'README.md')

$manifestFiles = Get-ChildItem -LiteralPath (Join-Path $stagingRoot 'bambu-printer-overlay') -Recurse -File | ForEach-Object {
    [pscustomobject]@{
        path = $_.FullName.Substring($stagingRoot.Length + 1).Replace('\', '/')
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        bytes = $_.Length
    }
}
$manifest = [pscustomobject]@{
    name = 'bambu-printer-overlay'
    version = $version
    architecture = 'windows-x64'
    files = @($manifestFiles)
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stagingRoot 'manifest.json') -Encoding UTF8

& (Join-Path $PSScriptRoot 'validate-package.ps1') -PackageRoot $stagingRoot
if ($LASTEXITCODE -ne 0) { throw 'Package validation failed.' }

$zipPath = Join-Path $releaseRoot "$artifactBase.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $stagingRoot '*') -DestinationPath $zipPath -CompressionLevel Optimal

# A named compiler must exist: falling back here would change the installer that ships.
if ($InnoCompiler -and -not (Test-Path -LiteralPath $InnoCompiler -PathType Leaf)) {
    throw "Inno Setup compiler not found: $InnoCompiler"
}
if (-not $InnoCompiler) {
    $innoCandidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )
    $InnoCompiler = $innoCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if ($InnoCompiler) {
    & $InnoCompiler "/DMySourceRoot=$stagingRoot" "/DMyOutputDir=$releaseRoot" "/DMyVersion=$version" (Join-Path $nativeRoot 'installer\BambuPrinterOverlay.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
} else {
    $iexpress = Join-Path $env:WINDIR 'System32\iexpress.exe'
    if (-not (Test-Path -LiteralPath $iexpress -PathType Leaf)) {
        Write-Warning 'Neither Inno Setup nor Windows IExpress is available; only the portable ZIP was created.'
    } else {
        $installerWork = Join-Path $nativeRoot 'package-installer'
        Assert-ChildPath $installerWork $nativeRoot
        if (Test-Path -LiteralPath $installerWork) {
            Remove-Item -LiteralPath $installerWork -Recurse -Force
        }
        New-Item -ItemType Directory -Path $installerWork | Out-Null
        Copy-Item -LiteralPath (Join-Path $nativeRoot 'installer\bootstrap.ps1') -Destination $installerWork
        Copy-Item -LiteralPath $zipPath -Destination (Join-Path $installerWork 'payload.zip')

        $setupPath = Join-Path $releaseRoot "$artifactBase-Setup.exe"
        $sourcePath = $installerWork.TrimEnd('\') + '\'
        $sedPath = Join-Path $installerWork 'installer.sed'
        @"
[Version]
Class=IEXPRESS
SEDVersion=3

[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=0
HideExtractAnimation=0
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=%InstallPrompt%
DisplayLicense=
FinishMessage=
TargetName=%TargetName%
FriendlyName=%FriendlyName%
AppLaunched=%AppLaunched%
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=
SourceFiles=SourceFiles

[Strings]
InstallPrompt=Install Bambu Printer Overlay for all users of OBS Studio?
TargetName=$setupPath
FriendlyName=Bambu Printer Overlay $version
AppLaunched=powershell.exe -NoProfile -ExecutionPolicy Bypass -File bootstrap.ps1
FILE0=bootstrap.ps1
FILE1=payload.zip

[SourceFiles]
SourceFiles0=$sourcePath

[SourceFiles0]
%FILE0%=
%FILE1%=
"@ | Set-Content -LiteralPath $sedPath -Encoding ASCII

        if (Test-Path -LiteralPath $setupPath) { Remove-Item -LiteralPath $setupPath -Force }
        $iexpressProcess = Start-Process -FilePath $iexpress -ArgumentList @('/N', '/Q', $sedPath) -Wait -PassThru
        if ($iexpressProcess.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $setupPath -PathType Leaf)) {
            throw 'Windows self-extracting installer creation failed.'
        }
    }
}

$artifacts = Get-ChildItem -LiteralPath $releaseRoot -File | Where-Object {
    $_.BaseName -like "$artifactBase*" -and $_.Extension -in @('.zip', '.exe')
}
foreach ($artifact in $artifacts) {
    $hashPath = "$($artifact.FullName).sha256"
    $hash = (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    # sha256sum -c reads a carriage return as part of the file name, so this must end in LF.
    [IO.File]::WriteAllText($hashPath, "$hash  $($artifact.Name)`n", [Text.Encoding]::ASCII)
    Write-Output "Created $($artifact.FullName)"
    Write-Output "SHA-256 $hash"
}
