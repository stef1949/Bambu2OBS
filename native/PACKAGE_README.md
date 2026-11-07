# Bambu Printer Overlay

A native OBS Studio source showing live telemetry from a Bambu Lab 3D printer. It needs only OBS 33 or newer on 64-bit Windows.

This is a community plugin, not affiliated with or endorsed by OBS Project or Bambu Lab.

## Install

Close OBS, then either:

- run the `Setup.exe` installer as administrator, or
- right-click `install.ps1` in this folder, choose **Run with PowerShell**, and approve the prompt.

The default location is:

```text
C:\ProgramData\obs-studio\plugins\bambu-printer-overlay
```

For a portable OBS, pass the `plugins` folder in its install root:

```powershell
.\install.ps1 -ObsPluginRoot 'D:\OBS\plugins'
```

## Set up

1. In OBS open **Tools > Bambu Printer Overlay Settings**, press **Add**, and enter the printer's IP, serial number, and LAN access code.
2. Press **Save**, then **Test Connection**, and accept the printer's certificate.
3. Add a **Bambu Printer Overlay** source and choose your printer.

## Upgrade and uninstall

Close OBS before either. Installing a newer version replaces the plugin; the ZIP installer backs up the existing folder first. To remove it, run `uninstall.ps1` as administrator.

Profiles are kept in your OBS configuration folder and access codes in Windows Credential Manager. Removing a profile in the settings deletes its stored secrets.

## Troubleshooting

If the source is missing, open **Help > Log Files > View Current Log** and search for `bambu-printer-overlay`. Confirm OBS is 64-bit, version 33 or newer, and the DLL exists at:

```text
C:\ProgramData\obs-studio\plugins\bambu-printer-overlay\bambu-printer-overlay.dll
```

The installer is not code-signed, so Microsoft Defender SmartScreen may warn about it. Check the published SHA-256 checksum before running it.
