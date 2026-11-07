#Requires -Version 5.1

<#
.SYNOPSIS
Installs and uninstalls the built Setup.exe, and checks what each one did.

.DESCRIPTION
This replaces any real install of the plugin on the machine, so it runs only in
CI. It needs the Inno Setup installer, because the IExpress fallback doesn't take
these silent switches.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $ReleaseRoot,
    [string] $PluginsRoot = (Join-Path $env:ProgramData 'obs-studio\plugins')
)

$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true') {
    throw 'This replaces the installed plugin, so it runs only in GitHub Actions.'
}

$setup = @(Get-ChildItem -LiteralPath $ReleaseRoot -Filter '*-Setup.exe' -File)
$zip = @(Get-ChildItem -LiteralPath $ReleaseRoot -Filter '*.zip' -File)
if ($setup.Count -ne 1 -or $zip.Count -ne 1) {
    throw "Expected one installer and one ZIP in $ReleaseRoot, found $($setup.Count) and $($zip.Count)."
}

$pluginRoot = Join-Path $PluginsRoot 'bambu-printer-overlay'
if (Test-Path -LiteralPath $pluginRoot) {
    throw "The plugin is already installed at $pluginRoot."
}

$workRoot = Join-Path ([IO.Path]::GetTempPath()) ('bambu-installer-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $workRoot | Out-Null
try {
    Expand-Archive -LiteralPath $zip[0].FullName -DestinationPath $workRoot
    $packagedRoot = Join-Path $workRoot 'bambu-printer-overlay'

    # Hash every file relative to its root, so two trees compare as one sorted list.
    function Get-TreeHashes([string] $Root) {
        Get-ChildItem -LiteralPath $Root -Recurse -File | ForEach-Object {
            '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $_.FullName.Substring($Root.Length + 1)
        } | Sort-Object
    }

    $logPath = Join-Path $workRoot 'install.log'
    $install = Start-Process -FilePath $setup[0].FullName -Wait -PassThru -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$logPath`""
    )
    if ($install.ExitCode -ne 0) {
        if (Test-Path -LiteralPath $logPath) { Get-Content -LiteralPath $logPath | Write-Output }
        throw "The installer exited with code $($install.ExitCode)."
    }
    if (-not (Test-Path -LiteralPath $pluginRoot -PathType Container)) {
        throw "The installer did not create $pluginRoot."
    }

    $differences = Compare-Object @(Get-TreeHashes $packagedRoot) @(Get-TreeHashes $pluginRoot)
    if ($differences) {
        throw "The installed plugin does not match the ZIP:`n$($differences | Out-String)"
    }
    Write-Output "Installed $pluginRoot, matching the ZIP."

    $uninstaller = @(Get-ChildItem -LiteralPath $PluginsRoot -Filter 'unins*.exe' -File)
    if ($uninstaller.Count -ne 1) {
        throw "Expected one uninstaller in $PluginsRoot, found $($uninstaller.Count)."
    }
    $uninstall = Start-Process -FilePath $uninstaller[0].FullName -Wait -PassThru -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
    )
    if ($uninstall.ExitCode -ne 0) {
        throw "The uninstaller exited with code $($uninstall.ExitCode)."
    }

    # The uninstaller hands over to a copy of itself in Temp, so it can exit before it finishes.
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while ((Test-Path -LiteralPath $pluginRoot) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 500
    }
    if (Test-Path -LiteralPath $pluginRoot) {
        throw "The uninstaller left $pluginRoot behind."
    }
    Write-Output 'Uninstalled cleanly.'
} finally {
    Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
}
