# Changelog

All notable changes to Bambu2OBS will be documented here.

## Unreleased

### Added

- Printer TLS certificate pinning and an explicit insecure compatibility exception.
- Bounded cloud-image and `.3mf` preview processing with image signature validation.
- Runtime freshness states and consolidated overlay telemetry.
- Multi-AMS canonical output filenames while retaining first-AMS legacy aliases.
- Atomic telemetry, diagnostics, SVG, task-ID, and cover-image writes.
- CI across supported Python versions and regression coverage for security and parsing behavior.
- A Windows x64 native preview DLL, official-template-based dependency bootstrap, headless libobs load test, validated portable ZIP, and self-extracting installer.
- Hash-verifying install/uninstall scripts for recommended ProgramData and custom portable OBS plugin paths.

### Changed

- Cloud bearer tokens are sent only to the configured Bambu API host, never to third-party preview hosts.
- Cloud and SD-preview work no longer blocks the MQTT callback thread.
- Overlay assets are self-contained and no longer load Bootstrap from a CDN.
- Direct Python dependencies are pinned to the locally validated versions.
- Native package contents now use OBS's recommended `plugin/bin/64bit` and `plugin/data` layout and carry the pinned OBS GPL text.

### Fixed

- Numeric print-stage values now match `pybambu`'s integer-keyed stage table.
- Successful SD-card previews are no longer downloaded again every retry interval.
- SVG output respects an absolute or custom `BASE_DIR`.
- Progress-server startup is checked and subprocess shutdown is bounded.
