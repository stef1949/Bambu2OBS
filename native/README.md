# Native OBS plugin preview

This directory contains the installable Windows x64 preview of **Bambu Printer Overlay**. It follows the current OBS module/source conventions and produces a real `bambu-printer-overlay.dll` that OBS can load.

The preview includes:

- stable source ID `bambu_printer_overlay`;
- `OBS_DECLARE_MODULE()`, localization, and registration from `obs_module_load()`;
- synchronous transparent native rendering;
- localized source properties with an explicit preview warning;
- typed snapshot bounds, delta retention, revision, and staleness logic;
- headless libobs loading/source-creation integration tests;
- ProgramData-compatible ZIP and EXE installer artifacts;
- package validation for layout, x64 PE architecture, credentials, developer paths, and forbidden Python runtime files.

## Current scope

The installed source currently renders a native simulation progress card. It does not connect to a printer. Live MQTT, FTPS/cloud previews, credential profiles, complete text/image rendering, shared services, and all three finished visual presets remain to be implemented.

This is an installation and native-integration preview, not a production-ready replacement for the working Python implementation. That limitation is shown in the package README and inside the OBS source properties.

## Install the preview

Build artifacts are written to the repository's `release` directory:

```text
Bambu-Printer-Overlay-0.2.0-preview.1-Windows-x64-Setup.exe
Bambu-Printer-Overlay-0.2.0-preview.1-Windows-x64.zip
```

Close OBS, run the unsigned preview installer as administrator, restart OBS, then add **Bambu Printer Overlay** from the Sources menu. The installer targets:

```text
C:\ProgramData\obs-studio\plugins\bambu-printer-overlay
```

The portable ZIP contains `install.ps1`, `uninstall.ps1`, a manifest, and the same validated plugin payload. See [PACKAGE_README.md](PACKAGE_README.md).

## Reproducible Windows build

Requirements:

- Visual Studio 2022 Build Tools with the x64 C toolchain;
- Windows SDK 10.0.20348 or newer;
- CMake 3.28 or newer;
- Git and internet access for the first dependency bootstrap.

The preset pins the official OBS plugin-template commit, OBS 31.1.1 sources, the 2025-07-11 OBS dependency bundle, and SHA-256 hashes. The first configure downloads and builds libobs; later builds reuse `native/.deps`.

```powershell
cd native
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --preset windows-x64
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1 -Configuration Release
```

The current preview has also been linked and tested on the repository machine with installed OBS Studio 32.2.1. The package does not bundle OBS.

## Core-only build

The typed snapshot core remains buildable without OBS:

```powershell
cd native
cmake --preset core-tests
cmake --build --preset core-tests
ctest --preset core-tests
```

## Package contents

The portable archive uses OBS's recommended per-machine layout:

```text
bambu-printer-overlay/
  bin/64bit/bambu-printer-overlay.dll
  data/locale/en-US.ini
  data/licenses/
install.ps1
uninstall.ps1
manifest.json
README.md
```

No Python interpreter, Python source, Flask server, Browser Source, localhost service, credentials, OBS binaries, or developer build tree is included.

## Next native milestones

1. Implement sanitized MQTT fixtures, parsing, reconnect scheduling, and a shared printer-service registry.
2. Add Windows Credential Manager storage and per-profile certificate pinning.
3. Port cloud and bounded FTPS `.3mf` preview retrieval.
4. Render complete localized typography, artwork, temperatures, layers, status, and multiple AMS units.
5. Finish Compact, Dashboard, and Expanded presets with visual snapshot tests.
6. Validate duplicate sources, hide/show, reconnect, in-flight shutdown, soak behavior, and a real printer.
7. Obtain owner approval for the stable native licensing/publication approach and code-sign release installers.

The complete acceptance boundary remains in [`../OVERLAY_OVERHAUL_PROMPT.md`](../OVERLAY_OVERHAUL_PROMPT.md).
