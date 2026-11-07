#Requires -Version 5.1

$ErrorActionPreference = 'Stop'
$currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($currentIdentity)
$isAdministrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdministrator) {
    $arguments = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $PSCommandPath)
    )
    try {
        $elevated = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        exit $elevated.ExitCode
    } catch {
        Write-Error "Installation was not completed: $($_.Exception.Message)"
        exit 1
    }
}

$payloadPath = Join-Path $PSScriptRoot 'payload.zip'
if (-not (Test-Path -LiteralPath $payloadPath -PathType Leaf)) {
    throw 'The installer payload is missing.'
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("BambuPrinterOverlay-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
try {
    Expand-Archive -LiteralPath $payloadPath -DestinationPath $temporaryRoot
    & (Join-Path $temporaryRoot 'install.ps1')
    if ($LASTEXITCODE -ne 0) {
        throw "Plugin installation failed with exit code $LASTEXITCODE."
    }
    Add-Type -AssemblyName PresentationFramework
    [System.Windows.MessageBox]::Show(
        'Bambu Printer Overlay was installed. Start OBS Studio and add Bambu Printer Overlay as a source.',
        'Bambu Printer Overlay',
        'OK',
        'Information'
    ) | Out-Null
} finally {
    $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
    $systemTemporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolvedTemporaryRoot.StartsWith($systemTemporaryRoot, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
