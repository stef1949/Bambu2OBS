# Native OBS plugin foundation

This directory contains the first buildable foundation for the self-contained **Bambu Printer Overlay** source. It follows the current OBS module/source conventions:

- stable source ID `bambu_printer_overlay`;
- `OBS_DECLARE_MODULE()` and default locale support;
- registration from `obs_module_load()`;
- synchronous transparent custom rendering;
- localized source properties;
- an OBS-independent typed snapshot core with bounds and staleness tests;
- installer-friendly `bambu-printer-overlay/bin/64bit` and `data` install destinations.

## Current scope

The source currently renders a native simulation progress card. It does not yet connect to a printer. It is deliberately kept alongside the working Python implementation while MQTT, FTPS, cloud metadata, credentials, shared services, text/image rendering, and lifecycle behavior are implemented and tested.

Do not describe this foundation as a production-ready plugin or distribute it as a completed replacement.

## Build core tests without OBS

With a C11 compiler:

```powershell
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror `
  native/src/printer-snapshot.c native/tests/test-printer-snapshot.c `
  -I native/src -o native/build/bambu-overlay-core-tests.exe
.\native\build\bambu-overlay-core-tests.exe
```

## Build the OBS module

Use CMake 3.28–3.30 and a supported libobs development package or the current official `obsproject/obs-plugintemplate` dependency workflow:

```powershell
cmake -S native -B native/build -G "Visual Studio 17 2022"
cmake --build native/build --config Release
ctest --test-dir native/build -C Release --output-on-failure
cmake --install native/build --config Release --prefix native/dist
```

This machine did not have CMake or the libobs SDK available when the foundation was added, so only the independent snapshot core can be compiled locally here. A release must still be built and loaded in a supported OBS installation.

## Next native milestones

1. Add sanitized MQTT fixtures and a delta-state reducer around `bambu_printer_snapshot`.
2. Add a shared, cancellation-aware printer service outside render callbacks.
3. Add OS credential storage and the certificate trust model.
4. Render localized text, preview art, temperatures, layers, status, and multiple AMS units.
5. Add Compact, Dashboard, and Expanded layout snapshots.
6. Integrate the full official plugin-template build, packaging, and release workflow.
7. Validate duplicate sources, hide/show, reconnect, in-flight shutdown, soak behavior, and packaged installation.

The complete acceptance boundary remains in [`../OVERLAY_OVERHAUL_PROMPT.md`](../OVERLAY_OVERHAUL_PROMPT.md).
