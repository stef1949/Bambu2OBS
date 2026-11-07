# Architecture

This records why the plugin is built the way it is. For build instructions see [`native/README.md`](../native/README.md).

## Decision: one native source with no bundled runtime

The goal is that a user installs one thing and configures it inside OBS. So the plugin is a single `bambu-printer-overlay.dll` that imports only OBS, the Qt and VC++ runtimes OBS already depends on, and components of Windows. There are no bundled DLLs, no font or image assets, and no helper process.

| Need | Choice | Why |
| --- | --- | --- |
| Printer TLS (MQTT 8883, FTPS 990) | Schannel | Part of Windows. Resumes sessions across connections sharing a credential handle, which FTPS data channels require. |
| Cloud HTTPS | WinHTTP | Part of Windows, with full default certificate validation and system proxy support. |
| Secrets | Windows Credential Manager | Keeps secrets out of scene collections, profile files, and logs. |
| Image decoding | Windows Imaging Component | A maintained, patched decoder for untrusted image data. |
| Text | GDI with system fonts | No font files to license or ship, and Windows font fallback handles CJK job names. |
| Rendering | CPU compositor, one texture upload | Deterministic, testable without a GPU, and costs nothing when nothing changes. |
| Settings UI | Qt, via `obs-frontend-api` | Takes OBS's own theme, and Qt ships with OBS. See below. |
| JSON, MQTT, ZIP | Small bounded C modules | The plugin needs narrow slices of each, and all of them parse untrusted input with explicit limits. |

The cost of this is that the plugin is Windows-only. That is the declared target. The portable core (`bambu-overlay-core`) has no platform dependencies, so another platform would need to supply only the Windows-specific layer.

## Decision: a Qt settings dialog

The dialog was first written in Win32 to avoid depending on Qt, but it looked out of place beside OBS and couldn't follow its theme. Qt is the toolkit OBS itself uses, so a Qt dialog matches every OBS theme with no styling code, and adds nothing to the package. WinUI 3 was rejected because it would have added a runtime to ship, and it has no supported way to run inside a Qt host.

Qt brings four obligations:

- **One C runtime.** Qt deletes child widgets from inside its own DLL, so the plugin must use the same shared runtime as Qt. A static runtime would free plugin memory through the wrong heap. OBS already requires that runtime, so users gain no dependency.
- **A Qt version link.** The plugin builds against the Qt in the pinned SDK and runs on the newer Qt in OBS, which relies on Qt's forward binary compatibility. It holds within Qt 6, so a future OBS on Qt 7 would need a rebuild.
- **No nested event loops.** The dialog and the trust prompt use `open()`, never `exec()`. Plugin code on the stack of an event loop would still be there when OBS unloads the module.
- **The dialog must not outlive the plugin.** Qt's main window can outlive the module, so `bambu_settings_shutdown()` destroys the dialog during unload, before a later delete could call into unloaded code.

Widgets also only work on the UI thread, and obs-websocket can press the source's **Open connection settings** button from its own threads, so `bambu_settings_open()` marshals to the UI thread.

The rest of the plugin stays C. Only `bambu-settings-qt.cpp` and its test are C++, and they contain no connection or credential logic.

## Layers

```text
OBS source adapter     bambu-overlay-source.c, plugin-main.c, bambu-settings-qt.cpp
Printer service        printer-service.c  (one per profile, shared, owns the threads)
Protocol clients       bambu-tls-win.c, bambu-ftps-win.c, bambu-http-win.c
Windows services       bambu-credentials-win.c, bambu-image-win.c, text-gdi-win.c, bambu-profiles.c
Portable core          bambu-report.c, printer-snapshot.c, bambu-mqtt-codec.c, bambu-json.c,
                       bambu-zip.c, bambu-preview-select.c, overlay-render.c, overlay-canvas.c, ...
```

Each layer depends only on those below it. Everything in the core can be tested with no OBS, no network, and no Windows API.

## Threading

OBS's render and UI threads must never wait on the network, so nothing on them does.

- **Connection thread** (one per service): TLS, MQTT, reconnect backoff. Reduces messages into a snapshot.
- **Preview thread** (one per service): cloud metadata, FTPS, archive parsing, image decoding. Running on one thread is what guarantees a single attempt at a time per printer.
- **Worker thread** (one per source): composes frames on the CPU. It alone holds the source's service reference, so a last-reference join cannot stall the UI thread that changed a setting.
- **`video_tick`** decides whether to wake the worker: every frame while animating, otherwise about once a second.
- **`video_render`** uploads a finished frame if one is waiting, then draws one sprite.

Every network call polls a cancel token in 100 ms slices, so workers can always be joined promptly. WinHTTP calls are aborted by closing the in-flight handle. `bambu_service_release` joins the workers before it returns when it drops the last reference, and source destroy joins the worker before freeing anything.

A source that nothing changed does no work: the worker compares a small render key (revision, visual state, clock minute, displayed progress) and returns without drawing.

## Sharing

Sources sharing a profile share one service, reference-counted in a registry. A source holds its reference for its lifetime rather than while visible, so scene switching causes no reconnects. A hidden source keeps its connection but composes nothing.

## Trust model

Printers present device certificates that chain to no public CA, so ordinary validation cannot work and disabling it would accept anyone. The plugin pins instead:

1. **Test Connection** opens a TLS connection and reads the certificate. It sends no credentials.
2. The user sees the SHA-256 fingerprints and decides whether to trust them.
3. Only then is the access code sent, and only over a connection that matched the pin.

A profile with no pin does not connect at all. A pin mismatch stops the connection with a visible error and no retries, since it may mean something is impersonating the printer. Changing a profile's host clears its pins. MQTT and FTPS are pinned separately because nothing guarantees they present the same certificate.

Cloud traffic is a separate trust domain. WinHTTP keeps full validation, redirects are refused because a redirect could reach an address that URL validation rejected, and the bearer token only ever goes to the API host, never to a CDN.

## Data freshness

`connecting`, `live`, `stale`, `reconnecting`, `offline`, `authentication failed`, and `configuration required` are distinct and visible. Staleness uses a monotonic clock.

A keepalive reply counts as freshness. P1-series printers can stay silent for long periods when idle, and a healthy link with no news is not stale data. A frozen value is never shown as live: stale progress loses its accent colour, and the clock ETA disappears because it would be a guess.

## Readability on stream

Overlays are watched after 720p scaling and low-bitrate compression, where text under about 12 px turns to smudges, so sharp renders are not the test. The smallest text is `BAMBU_TEXT_MIN`, 18 px at preset size, which is 12 px on a 720p stream from a 1080p canvas.

Text keeps its size and detail gives way. A source under `BAMBU_READABLE_FIT` of its preset size steps down a tier (panel, compact, minimal), and tray chips go from name and slot to a short material name to colour only. `bambu-overlay-render-tests` checks the floor at preset size and across the allowed source sizes.

The plugin can't see a scene item's transform, so it can't correct for a source scaled down in the scene.

## Colour

The canvas composes in sRGB. `bambu_canvas_encode` then rewrites each pixel as colour premultiplied in linear light, sRGB encoded, which is the form OBS expects from an sRGB texture blended with `GS_BLEND_ONE` / `GS_BLEND_INVSRCALPHA`. Without this, partly transparent edges come out too dark.

Blending happens in linear light, so opacity looks thinner than the same number would in sRGB. That is why the default panel opacity is 94% rather than something lower.

## Licensing

OBS is GPL-2.0-or-later, so a distributed binary linking libobs is covered by the GPL. The repository's source remains MIT, which is GPL-compatible, and the existing licence was not changed. Packages include both licence texts. See [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

The plugin links Qt at runtime, using the copy that ships with OBS, and links no other third-party code. Stage and filament identifier tables were derived from pybambu (Apache-2.0) and are attributed there. The AGPL-licensed Python cloud client is not used by the plugin; the few REST calls it needs are written directly.

## Differences from the Python version

These are deliberate:

- **No email/password login.** The plugin takes a cloud token only, because a password flow would need interactive verification codes and a second secret to store.
- **No insecure TLS mode.** Pinning replaces `ALLOW_INSECURE_PRINTER_TLS`.
- **`stg_cur` is preferred for the stage.** Python mapped `mc_print_stage` through the `stg_cur` table, and those are different fields. The plugin uses `mc_print_stage` only if `stg_cur` has never been seen.
- **It requests a full status once per connection.** Python never sent `pushall`, which leaves P1-series printers blank until something changes.
- **Cloud titles are not applied to mismatched LAN prints.** Python applied the newest cloud task regardless, which could mislabel a local job.
- **Preview retries are capped** at six per job, and also search the printer's `cache` directory.
