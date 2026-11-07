# Changelog

All notable changes to Bambu2OBS will be documented here.

## 0.3.0

### Added

- Self-contained native plugin: live telemetry entirely inside OBS as a single DLL that needs only OBS and Windows.
- LAN MQTT over TLS with certificate pinning, reconnect backoff, and one shared connection per printer.
- Print previews from Bambu Cloud or from the printer's `.3mf` over implicit FTPS, with a bounded cache.
- Compact, Dashboard, and Expanded presets; dark, light, high-contrast, and custom themes; multi-AMS display; deterministic simulation mode.
- **Tools > Bambu Printer Overlay Settings** for connection profiles, a Qt dialog that follows the OBS theme, with secrets in Windows Credential Manager, a connection test, and redacted diagnostics.
- Integration tests against a local fake printer, automated settings-dialog tests, and libobs tests with D3D11 pixel readback.
- Architecture, troubleshooting, migration, and legacy Python documentation.
- Printer TLS certificate pinning and an explicit insecure compatibility exception.
- Bounded cloud-image and `.3mf` preview processing with image signature validation.
- Runtime freshness states and consolidated overlay telemetry.
- Multi-AMS canonical output filenames while retaining first-AMS legacy aliases.
- Atomic telemetry, diagnostics, SVG, task-ID, and cover-image writes.
- CI across supported Python versions and regression coverage for security and parsing behavior.
- A Windows x64 native preview DLL, official-template-based dependency bootstrap, headless libobs load test, validated portable ZIP, and self-extracting installer.
- Hash-verifying install/uninstall scripts for recommended ProgramData and custom portable OBS plugin paths.
- A release workflow: pushing a version tag builds from clean, installs and uninstalls the real installer, then publishes it with the ZIP and SHA-256 checksums.

### Changed

- Overlay text is now at least 18 px at preset size, so it stays readable on a 720p stream. Small sources drop detail instead of shrinking text, tray chips shorten their labels, material names lose the `Bambu` prefix, and the stage now comes before the print profile.
- Cloud bearer tokens are sent only to the configured Bambu API host, never to third-party preview hosts.
- Cloud and SD-preview work no longer blocks the MQTT callback thread.
- Overlay assets are self-contained and no longer load Bootstrap from a CDN.
- Direct Python dependencies are pinned to the locally validated versions.
- Native package contents now use the OBS 33 `plugin/plugin.dll` and `plugin/data` layout, because OBS flags `bin/64bit` as Legacy, and carry the pinned OBS GPL text. OBS 33 is now the minimum version.

### Fixed

- Profiles are locked during **Test Connection**, so one printer's certificate can no longer be pinned to another by switching profile mid-test.
- The overlay no longer shows the same word twice, as in `Printing · Printing`, when a print has no profile name.
- `.sha256` files ended in CRLF, so `sha256sum -c` reported good downloads as failed.
- A silent install hung on a message box when OBS was running. It now exits with an error.
- `native/data` was excluded by the `data/` ignore rule, so a fresh clone could not package the plugin.
- The Inno Setup check for a running OBS looked for a Qt 5 window class and could never match.
- Numeric print-stage values now match `pybambu`'s integer-keyed stage table.
- Successful SD-card previews are no longer downloaded again every retry interval.
- SVG output respects an absolute or custom `BASE_DIR`.
- Progress-server startup is checked and subprocess shutdown is bounded.
