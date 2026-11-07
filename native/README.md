# Native plugin

Contributor documentation for **Bambu Printer Overlay**. Users need none of this: see the [main README](../README.md). Design decisions are in [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).

## Build

Requirements:

- Visual Studio 2022 Build Tools with the x64 C and C++ toolchain, and Windows SDK 10.0.20348 or newer
- CMake 3.28 or newer
- Git, and internet access for the first dependency bootstrap
- Python 3 and OpenSSL, for the integration test only

```powershell
cd native
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --preset windows-x64
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package-windows.ps1 -Configuration Release
```

The preset pins the official OBS plugin-template commit, OBS 31.1.1 sources, the 2025-07-11 OBS dependency and Qt bundles, and their SHA-256 hashes. The first configure downloads and builds libobs into `native/.deps`, and later builds reuse it. Artifacts land in the repository's `release` directory.

To run the plugin load test against an installed OBS, which also enables the GPU checks:

```powershell
cmake --preset windows-x64 -DBAMBU_OVERLAY_TEST_OBS_DIR="C:/Program Files/obs-studio"
```

The core and its tests build without OBS:

```powershell
cmake --preset core-tests
cmake --build --preset core-tests
ctest --preset core-tests
```

## Tests

| Test | Covers |
| --- | --- |
| `bambu-overlay-core-tests` | Snapshot bounds, revisions, staleness |
| `bambu-overlay-protocol-tests` | JSON, MQTT codec, delta reduction, mappings, formatting, backoff, redaction, FTP listings, archive selection, cloud parsing, URL validation, inflate, `.3mf` limits |
| `bambu-overlay-render-tests` | Layout at many sizes and every section combination, visual states, motion, compositing, simulation fixtures |
| `bambu-overlay-service-tests` | The real TLS, MQTT, and FTPS clients against `tests/fake_printer.py`: pinning, auth failure, reconnect, sharing, cancellation, previews, Credential Manager, profiles |
| `bambu-overlay-dialog-tests` | The real Qt settings dialog driven through its widgets: layout, secrets, opening from another thread, cancelling a test, shutdown, and every locale key it uses |
| `bambu-overlay-dialog-trust-tests` | The same dialog against a real TLS server: the certificate prompt, and that the access code is not sent before approval |
| `bambu-overlay-plugin-load-tests` | The real DLL in real libobs: registration, properties, legacy settings, lifecycle churn, and with a graphics device, D3D11 rendering with pixel readback |

`bambu-overlay-gallery <directory>` renders every preset and state to PNG. Use it to review visual changes, and to regenerate the images in `images/`.

Judge visual changes as a viewer sees them, not from the sharp PNGs. Put the render on a 1080p canvas over a busy background, scale to 720p, and encode at about 2500 kbps with x264 `veryfast` and 4:2:0 colour. Do not add text below `BAMBU_TEXT_MIN`; see [Readability on stream](../docs/ARCHITECTURE.md#readability-on-stream).

The fake printer rejects FTPS data connections that do not resume the control session, because real printers are reported to do the same. Keep that behaviour: it caught a real shutdown bug in the TLS client.

## Package layout

```text
bambu-printer-overlay/
  bambu-printer-overlay.dll
  data/locale/en-US.ini
  data/licenses/
  data/README.md
install.ps1
uninstall.ps1
manifest.json
README.md
```

This is the [OBS 33 plugin layout](https://obsproject.com/kb/legacy-plugin-locations). OBS flags the older `bin/64bit` layout as Legacy, and OBS before 33 does not find this one, so 33 is the minimum version even though the plugin builds against the 31.1.1 SDK.

`scripts/validate-package.ps1` rejects any other entry beside the DLL, wrong architecture, Python files, developer paths, and credential-like assignments.

## Releasing

`.github/workflows/release.yml` builds and tests from clean, installs and uninstalls the real `Setup.exe`, then publishes it with the ZIP and their `.sha256` files.

1. Set the version in `buildspec.json` and `CMakeLists.txt`.
2. In `CHANGELOG.md`, move the **Unreleased** entries under a `## <version>` heading.
3. Commit that, then push a tag:

   ```powershell
   git tag v0.3.0
   git push origin v0.3.0
   ```

A suffix such as `v0.3.0-rc.1` publishes a prerelease. To review a release first, run the **Release** workflow by hand with the tag name. That makes a draft, and publishing the draft creates the tag.

The workflow stops before building if the tag, version, and changelog disagree, and it never replaces an existing release.

## Validation status

Verified on Windows 11, x64, with Release builds:

- All tests above pass against OBS 31.1.1 with Qt 6.8.3 (SDK build, no graphics) and OBS 33.0.1 with Qt 6.11.1 (installed build, D3D11).
- A clean configure bootstraps Qt, libobs, and the OBS dependencies from the pinned URLs and hashes.
- Ten deliberate faults in the settings dialog were each caught by the tests, including skipping the trust prompt, logging in after a decline, and defaulting the prompt to Yes.
- The packaged ZIP, installed by its own `install.ps1` into a folder outside the repository, loaded and rendered in OBS 33.0.1.
- In a portable copy of OBS 33.0.1 running the real frontend: `install.ps1` upgraded an old `bin/64bit` install, OBS found and loaded the plugin itself, the Plug-in Manager showed no Legacy flag, **Tools > Bambu Printer Overlay Settings** opened and closed the dialog, OBS reported 0 memory leaks on exit, and `uninstall.ps1` removed the plugin.
- In the same portable OBS, the dialog took the Yami theme, opened from an obs-websocket thread and over a source's properties, and a printer added there appeared in the open properties. Quitting with it open exited cleanly.
- Through that real UI against `tests/fake_printer.py` on ports 8883 and 990: the trust prompt appeared with 0 logins received, approving it connected, and the source then showed live MQTT data and an FTPS preview.
- OBS 33.0.1 reports 1 memory leak on any restart with a saved scene, with or without this plugin installed. It reports 0 on a first run.
- At 1920x1080, 30 fps, on OBS 33.0.1, measured as process CPU over 10 seconds: 2.97% of one core with no source, 2.97% with a settled overlay, and 3.28% with an overlay changing every second. Working set grew 0.2 to 0.3 MB per interval during those runs.
- `obs_shutdown` took under 160 ms in every run.

**Not yet verified:**

- **A physical printer.** MQTT, FTPS, and cloud behaviour are tested only against `tests/fake_printer.py` and the payload formats the Python implementation handles. The riskiest assumption is FTPS session resumption between Schannel and the printer's TLS stack. Cloud endpoints have parsing tests but have not been called.
- **Multi-hour soak.** No long-duration run has been done yet.
- **CI.** The Qt build has not yet run on GitHub Actions, including the dialog tests, which need a desktop session. The release workflow has not run either.
- **The real `Setup.exe`.** It needs elevation and writes to the real OBS plugin folder, so only CI installs it, and CI has not run it yet. A per-user build of the same `.iss`, compiled with Inno Setup 6.7.1, installed files matching the ZIP and uninstalled cleanly.
