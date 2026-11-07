# Bambu Printer Overlay 0.2.0 Preview

This package installs the native **Bambu Printer Overlay** source for OBS Studio on Windows x64.

> Preview limitation: this build renders deterministic simulation data only. It proves the native OBS module, source registration, rendering, properties, installation layout, and unload path. Live printer MQTT, preview retrieval, and connection-profile support remain in the legacy Python application and are not included in this native package.

The project is community-maintained and is not affiliated with or endorsed by OBS Project or Bambu Lab.

## Installer

1. Close OBS Studio.
2. Run `Bambu-Printer-Overlay-0.2.0-preview.1-Windows-x64-Setup.exe` as an administrator.
3. Start OBS Studio.
4. Add a source and select **Bambu Printer Overlay**.

## Portable ZIP

1. Extract the ZIP.
2. Close OBS Studio.
3. Right-click `install.ps1`, choose **Run with PowerShell**, and approve elevation if requested.
4. Start OBS Studio and add **Bambu Printer Overlay** as a source.

The default destination is:

```text
C:\ProgramData\obs-studio\plugins\bambu-printer-overlay
```

For a portable or custom OBS installation, run an elevated PowerShell and provide the plugin root:

```powershell
.\install.ps1 -ObsPluginRoot 'D:\OBS\config\obs-studio\plugins'
```

## Upgrade and uninstall

Close OBS before upgrading. Running a newer installer replaces the plugin files. The ZIP installer backs up an existing folder before replacement.

Use **Installed apps** for the EXE installer, or run the ZIP package's `uninstall.ps1` as administrator.

## Troubleshooting

If the source is absent, open **Help > Log Files > View Current Log** and search for `bambu-printer-overlay`. Confirm that OBS is 64-bit and that the DLL exists at:

```text
C:\ProgramData\obs-studio\plugins\bambu-printer-overlay\bin\64bit\bambu-printer-overlay.dll
```

Unsigned preview installers can trigger Microsoft Defender SmartScreen. Review the published SHA-256 checksum before running the installer.
