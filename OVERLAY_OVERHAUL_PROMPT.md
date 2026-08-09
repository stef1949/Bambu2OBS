# Bambu2OBS Native Overlay Overhaul — Implementation Prompt

You are a senior OBS Studio plugin engineer, C/C++ systems developer, broadcast-graphics designer, and release engineer. Work directly in the existing Bambu2OBS repository. Your job is to replace its current Python/Flask/browser-source overlay architecture with a polished, installable, self-contained OBS Studio plugin.

Do not stop after analysis, an architecture proposal, a visual mock-up, or scaffolding. Inspect the repository, implement the overhaul end to end, test it, package it, and document it. If a live printer or local OBS installation is unavailable, use deterministic fixtures and a simulator for everything that can be verified, clearly record the remaining live validation, and still produce a buildable release candidate.

## Primary outcome

Deliver a native OBS input source named **Bambu Printer Overlay** that users can install, configure, and run entirely from OBS Studio.

After installing the release package, a normal user must only need to:

1. Open OBS Studio.
2. Open the plugin's connection settings and add their printer.
3. Add **Bambu Printer Overlay** as a source.
4. Choose their printer profile and visual preset.

The installed plugin must not require Python, pip, a virtual environment, Flask, Node.js, npm, a terminal, VS Code, a manually started helper process, a localhost web server, a Browser Source, online CSS/fonts/icons, or editable absolute file paths.

Treat “self-contained” as a release-runtime requirement, not merely a development convenience. Build tools are allowed for contributors; they must not be required on an end user's machine.

## Repository facts you must understand before changing anything

Inspect the real files and current working tree before implementation. At minimum, read:

- `README.md`
- `requirements.txt`
- `example.env`
- `src/bambu2obs.py`
- `src/progressbarServer.py`
- `src/templates/Filaments.svg`
- `src/templates/ActiveFilament.svg`
- `obs_template/Bambu2OBS_Scene.json`
- all files under `tests/`
- `LICENSE`

Run `git status`, inspect all diffs, and preserve unrelated or unfinished user changes. At the time this prompt was written, `src/bambu2obs.py` and `src/progressbarServer.py` had working-tree modifications; do not overwrite or discard them. Treat the current working tree as the source of truth.

The current architecture:

- runs `src/bambu2obs.py` as a Python process;
- connects to a Bambu Lab printer through local TLS MQTT on port 8883;
- optionally uses Bambu Cloud metadata;
- uses implicit FTPS on port 990 and `.3mf` metadata images as a preview fallback;
- writes live state to many `.txt`, `.json`, `.png`, and generated `.svg` files;
- starts `src/progressbarServer.py` as a Flask subprocess;
- exposes overlay and diagnostic routes on `http://localhost:5000`;
- renders the progress UI as HTML, currently including Bootstrap from a CDN;
- requires an OBS scene made from many text, image, SVG Browser, and localhost Browser sources;
- contains hard-coded machine-specific Windows paths in the example scene.

Preserve the useful behavior, not that architecture.

## Existing data and behavior that must survive

Port the current behavior into a typed in-memory model. Do not use generated text files as live IPC. Support partial/delta MQTT messages and retain the latest known value for fields omitted from later messages.

At minimum, preserve:

- connection state and last-update time;
- printer/job state;
- job/design title;
- print profile name;
- print stage;
- progress percentage, clamped safely to 0–100;
- remaining time;
- current layer and total layers;
- nozzle temperature;
- bed temperature;
- fan percentage;
- print-speed profile;
- AMS tray number, active tray, material name/type, material ID, and colour;
- total estimated print time and weight when cloud metadata provides them;
- cover/thumbnail retrieval from cloud metadata when enabled;
- local/SD-card `.3mf` preview fallback through implicit FTPS;
- placeholder artwork when no cover is available;
- resilient unknown-value handling when a printer firmware introduces a new stage, filament, or speed ID.

Port or replace the mappings currently obtained from `pybambu`, including speed profiles, stage IDs, and filament names. Back them with tests and always retain a readable unknown fallback. Do not require any Python package at runtime.

Handle more than one AMS unit in the data model when the payload contains them. The default visual preset may prioritise four trays, but the implementation must not corrupt or silently mislabel data from additional units. Provide an appearance option to show the first AMS, the active AMS, or all available trays in a responsive layout.

Do not absorb the user's webcam into the plugin. This source is a transparent overlay intended to sit above any camera or printer feed in an OBS scene.

## Non-negotiable architecture

Use the current official `obsproject/obs-plugintemplate` conventions and its CMake/release workflow as the foundation, adapted carefully into this repository rather than destructively reinitialising it.

Implement:

- a native plugin module with `OBS_DECLARE_MODULE()`;
- `OBS_MODULE_USE_DEFAULT_LOCALE(...)` and at least `data/locale/en-US.ini`;
- an `OBS_SOURCE_TYPE_INPUT` source registered from `obs_module_load()`;
- stable source and settings IDs so scene collections remain compatible across updates;
- appropriate `obs_source_info` callbacks for defaults, properties, create, update, destroy, dimensions, tick, render, activate/deactivate, and show/hide where useful;
- a synchronous video source with transparent output and native libobs rendering;
- `OBS_SOURCE_CUSTOM_DRAW` only if the renderer actually performs custom drawing, with correct graphics-context use;
- correct sRGB/colour-space handling for the supported OBS versions;
- plugin assets located with `obs_module_file()` or the corresponding module data-path API;
- mutable plugin cache/config located with `obs_module_config_path()` or the corresponding module config-path API;
- structured OBS logging through `blog()` with a consistent `[bambu-printer-overlay]` prefix;
- proper cleanup during source destruction and `obs_module_unload()`.

Do not implement the released overlay as a Python script, Lua script, Browser Source, WebView, embedded localhost HTTP server, out-of-process service, or executable that OBS must launch.

Network, TLS, HTTP, FTPS, archive parsing, image decoding, and reconnect work must never block OBS's render thread, UI thread, `video_tick`, `video_render`, or source property callbacks. Use owned worker threads/tasks, cancellation, bounded queues, timeouts, and immutable or safely synchronised snapshots. After `destroy` returns, no worker may access source memory or call `obs_source_output_video`.

Multiple overlay sources that select the same printer profile must share one connection/service rather than opening duplicate MQTT, FTPS, or cloud sessions. Source duplication and scene switching must not cause reconnect storms. Shut shared services down cleanly after their last consumer is gone.

Rendering resources must be created, updated, and destroyed in a valid OBS graphics context. Minimise GPU uploads: only rebuild text/image textures or geometry when the corresponding state or appearance setting changes. Never perform per-frame disk or network I/O.

Use maintained native dependencies suitable for redistribution. Pin versions, verify checksums where practical, make dependency ownership clear in CMake, and include licence notices. Prefer the official template's supported dependency/build mechanisms. Do not download floating `main` branches during a release build. Do not bundle OBS itself.

## Connection and credential design

Core live telemetry must work in LAN-only mode without Bambu Cloud or general internet access.

Create a plugin-level **Bambu Printer Overlay Settings** dialog under OBS's Tools menu. Connection profiles belong there; individual overlay sources should select a profile and contain appearance settings, not duplicate credentials.

Each connection profile should support:

- friendly profile name;
- printer IP/hostname;
- printer serial number;
- access code;
- LAN-only mode, enabled by default;
- optional Bambu Cloud region and token for cloud-only metadata;
- optional preview retrieval controls;
- explicit connection-test action;
- reconnect action;
- clear cached preview action;
- redacted diagnostic export.

Prefer a cloud token over email/password and do not make cloud authentication necessary for core telemetry. If email/password support is retained for compatibility, isolate it, label it as optional, and never expose it in source settings.

Use an operating-system credential-store abstraction so access codes and cloud secrets are not written into OBS scene collections, exported profiles, logs, crash diagnostics, or plain-text plugin configuration. The first release may implement Windows Credential Manager first if Windows x64 is the declared target, but keep the interface portable for macOS Keychain and Linux Secret Service implementations.

The printer's LAN MQTT service commonly uses a device/self-signed certificate. Do not silently apply the current Python behavior of globally disabling certificate verification. Implement an explicit, documented trust model such as certificate fingerprint pinning or a clearly labelled per-profile local-certificate exception with a warning. Never weaken TLS verification for unrelated cloud/HTTP traffic.

Redact access codes, bearer tokens, email addresses, serial numbers where appropriate, query strings, and authorization headers from all logs and diagnostic exports. Add automated redaction tests.

Use:

- bounded connect/read timeouts;
- exponential reconnect backoff with jitter and an upper bound;
- cancellation-aware network calls;
- a maximum cover-image download size;
- MIME/signature validation before image decoding;
- bounded `.3mf` archive size, member count, and uncompressed image size;
- exact, safe archive-member matching without filesystem path extraction;
- atomic cache writes;
- stale-data tracking based on a monotonic clock.

The UI must distinguish `connecting`, `live`, `stale`, `reconnecting`, `offline`, `authentication failed`, and `configuration required`. A connection failure must never crash or freeze OBS.

## State model

Create a clear separation such as:

- plugin module and service registry;
- printer connection/profile service;
- protocol clients for MQTT, optional cloud HTTP, and FTPS;
- payload parser and delta-state reducer;
- typed immutable `PrinterSnapshot`;
- preview cache/decoder;
- layout and appearance model;
- OBS source adapter;
- renderer;
- diagnostics/redaction layer.

Use names appropriate to the chosen language and template; the separation is the requirement, not these exact filenames.

The snapshot should carry optional typed values, data timestamps, source/provenance where helpful, and a monotonically increasing revision. Rendering code consumes a snapshot and must not parse raw JSON.

Keep protocol behavior independently testable without loading OBS. Use captured, sanitised MQTT fixtures that cover:

- full initial state;
- partial updates;
- idle, preparing, printing, paused, failed, finished, and offline states;
- missing and malformed values;
- unknown enum IDs;
- local/LAN prints without cloud metadata;
- one AMS and multiple AMS units;
- cover available, cover missing, corrupt cover, and preview change;
- disconnect/reconnect sequences.

## Visual overhaul

Replace the current generic Bootstrap bar and pile of separate scene sources with one coherent broadcast graphic.

Default art direction: a restrained, modern 3D-printer instrument panel—dark translucent surfaces, crisp typography, subtle depth, Bambu-green-inspired accent by default, excellent legibility over both bright and dark webcam footage, and motion that communicates state without distracting from the print. Do not use Bambu Lab or OBS logos unless their use is explicitly permitted. Do not imply official affiliation with either organisation.

Create three presets:

1. **Compact** — job, progress, percentage, ETA, and connection state.
2. **Dashboard** — the default; cover image, job/profile, progress, ETA, stage/layers, temperatures, fan/speed, and a four-tray AMS rail.
3. **Expanded** — adds total time/weight where available and supports multiple AMS units without becoming unreadable.

Use a responsive internal layout with a logical default size around 960×260 for Dashboard, but make width/height configurable within safe minimums. The source must render correctly when OBS scales or crops it and at common 16:9 canvases including 1280×720, 1920×1080, and 2560×1440.

Dashboard visual hierarchy:

- left: square print preview with a graceful bundled placeholder;
- centre: job name, secondary profile/stage line, large progress value, smooth horizontal progress track, ETA, and layer count;
- right: compact temperature, fan, and speed metrics;
- bottom: AMS tray rail with material name, colour swatch, unit/tray label, and unmistakable active-filament indication;
- edge/status area: a small live/stale/offline badge and last-update feedback when data is stale.

Appearance requirements:

- transparent source background outside the panel;
- configurable panel colour/opacity, accent colour, text colour, scale, corner radius, and padding;
- light, dark, and high-contrast themes;
- show/hide toggles for preview, AMS, stage, layer, temperatures, fan, speed, total time, total weight, and status badge;
- Celsius and Fahrenheit display without changing stored values;
- 12-hour/24-hour and concise ETA formatting where relevant;
- long job/profile/material names ellipsised or wrapped without overlap;
- no layout shift when optional fields disappear;
- readable placeholder glyphs for missing data;
- colour contrast that remains legible for common colour-vision deficiencies;
- active AMS state conveyed by more than colour alone;
- bundled/local, redistributable icons and fonts only, with attribution and licence files;
- no external image, font, JavaScript, or stylesheet requests.

Motion requirements:

- progress changes interpolate briefly and monotonically to the latest real value;
- state transitions use restrained fades/slides;
- completed, paused, error, stale, and offline states are visually distinct;
- animations run from OBS timing callbacks, not sleeps or busy loops;
- animations stop when settled and do not consume continuous CPU unnecessarily;
- provide a **Reduce motion** option that disables nonessential animation;
- never animate fake telemetry or imply precision the printer did not send.

Provide a deterministic demo/simulation mode in source properties for design and support testing. It must be off by default, clearly marked as simulated, contain no network side effects, and cycle through representative fixtures without exposing credentials.

## OBS properties and user experience

Group source properties clearly:

### Connection

- printer profile selector;
- read-only connection summary/status;
- shortcut button to open plugin connection settings;
- reconnect/test control where technically appropriate.

### Layout

- preset;
- logical width and height;
- scale/density;
- section visibility;
- AMS display mode.

### Appearance

- theme;
- accent, panel, and text colours;
- panel opacity;
- corner radius;
- font scale;
- reduced motion.

### Formatting

- temperature unit and decimal precision;
- time format;
- unknown/stale display behavior;
- stale threshold within safe bounds.

### Diagnostics

- simulation mode and fixture;
- read-only plugin version and last-update state;
- redacted diagnostic action.

Use the OBS properties API and localisation strings instead of hard-coded English UI text. `get_properties` must handle a null data pointer where the OBS API permits it. Defaults must yield an attractive, usable source without manual CSS or file editing.

## Reliability and performance

Design for an application that may stream for many hours.

Required behavior:

- safe startup when no profile exists;
- safe startup when the printer is powered off;
- reconnect when the printer returns;
- last-known display with explicit stale indication rather than frozen “live” data;
- no unbounded threads, queues, logs, caches, or reconnect attempts;
- no data races, use-after-free, detached worker leaks, shutdown hangs, or callbacks into destroyed OBS objects;
- no credential or raw full-payload spam in normal logs;
- rate-limited repeated warnings;
- bounded preview cache with invalidation on job change;
- exactly one active preview-generation/download attempt per job/profile;
- clean behavior when a source is hidden, shown, duplicated, renamed, removed, or included in multiple scenes;
- graceful OBS shutdown even while a network request is in progress.

Measure and report:

- idle CPU when the overlay is live but unchanged;
- CPU/GPU impact while animating;
- memory before and after a fixture soak;
- reconnect behavior under repeated failure;
- plugin load/unload and OBS shutdown time;
- whether each metric was measured in Debug or Release.

Optimise only after measuring, but reject any design that performs network work, filesystem polling, raw JSON parsing, font discovery, or full texture regeneration every frame.

## Build, packaging, and installation

The first-class release target is Windows x64, matching the user's environment, while keeping protocol/state/rendering code portable where reasonable.

Use the official plugin template's current supported Windows build environment and CMake presets. Do not invent stale OBS SDK paths or commit developer-specific absolute paths.

Produce:

- a reproducible Release build;
- CI for configure, build, unit tests, formatting/static checks, and packaging;
- a versioned Windows installer when supported by the official template workflow;
- a portable ZIP with the official recommended layout:
  - `bambu-printer-overlay/bin/64bit/bambu-printer-overlay.dll`
  - required redistributable dependency DLLs in the appropriate plugin binary directory;
  - `bambu-printer-overlay/data/locale/en-US.ini`
  - bundled shaders, icons, placeholder art, fonts, and notices under the plugin data directory;
- no Python interpreter, `.py` runtime, virtual environment, Flask files, or developer build tree in the release archive;
- no user credentials or test secrets in any artifact.

Validate the archive contents programmatically. Test installation into the recommended per-machine plugin directory:

`C:\ProgramData\obs-studio\plugins`

Also document custom/portable OBS paths. Do not use the legacy `C:\Program Files\obs-studio\obs-plugins\64bit` location as the primary install method.

An installed release must work after the repository and VS Code are closed. If runtime libraries are required, bundle or install them in a licence-compliant way so that supported OBS plus the plugin package are the only user-facing prerequisites.

## Migration and repository cleanup

Do not delete working Python behavior before the native implementation and fixtures reach parity.

Once parity is demonstrated:

- mark the Python/Flask path as legacy or move it to a clearly named legacy area if keeping it is valuable;
- exclude legacy Python from release packages;
- rewrite the README so normal installation starts with the installer/ZIP, not Python/pip;
- remove Flask, browser-source, localhost, Inkscape, VS Code, virtual-environment, and hard-coded path instructions from the primary user flow;
- replace the many-source scene template with simple instructions to add one native source;
- remove or archive the machine-specific scene JSON from the primary distribution;
- retain attribution for the original project and dependencies;
- add a migration guide for current Bambu2OBS users;
- add `CHANGELOG.md`, `SECURITY.md`, a troubleshooting guide, and third-party licence notices;
- include screenshots of all three presets generated from sanitised fixtures.

Do not silently change or remove the existing project licence. A proper OBS plugin links with GPL-licensed OBS code, so review the current OBS licensing requirements and make the plugin/repository GPL-compatible before release. Record the decision and preserve upstream attribution. Audit every bundled library, font, icon, and image for licence compatibility.

## Branding and publication compliance

Treat this as a third-party community plugin:

- state prominently that it is not affiliated with or endorsed by OBS Project or Bambu Lab;
- do not use the OBS name/logo or Bambu Lab marks as product artwork;
- do not claim official status;
- evaluate the existing `Bambu2OBS` repository/product naming against current OBS resource-branding rules before public submission, but do not rename the repository destructively without the owner's approval;
- keep the source public and release-ready if it will be submitted as an OBS resource;
- include accurate AI-assistance disclosure if AI tools contributed to code, art, documentation, or testing;
- warn the owner before forum submission if the current OBS resource policy may reject a plugin written entirely or largely by AI.

Do not fabricate compliance, testing, authorship, signing, notarisation, or security-review claims.

## Tests and validation

Add automated tests for at least:

- MQTT JSON parsing and delta-state reduction;
- numeric bounds and malformed values;
- stage/speed/filament mapping fallbacks;
- time, unit, layer, percentage, and temperature formatting;
- AMS indexing across one and multiple units;
- snapshot revision/staleness logic using an injected clock;
- reconnect backoff and jitter bounds using an injected scheduler/random source;
- secret redaction;
- cache keying and invalidation;
- cloud preview candidate selection;
- FTPS listing parsing, name matching, and TLS-session-reuse behavior;
- `.3mf` image selection and archive safety limits;
- image-size/type rejection;
- layout geometry at minimum/default/large source dimensions;
- long/missing/localised text;
- visual state selection for live/stale/offline/error/complete;
- reduced-motion behavior;
- shared-service reference counting;
- cancellation and destruction during in-flight work;
- settings migration from earlier plugin versions;
- release archive layout and absence of secrets/developer paths.

Add golden/snapshot image tests for Compact, Dashboard, and Expanded presets at representative resolutions if the rendering stack permits deterministic output. Review generated images for clipping, alpha, blurry text, colour contrast, and alignment.

Perform proportional integration testing:

1. Configure and build Debug and Release.
2. Run all unit/integration tests.
3. Load the Release plugin in a supported clean or portable OBS installation.
4. Add, duplicate, hide/show, resize, crop, and remove the source.
5. Exercise every visual state in simulation mode.
6. Verify transparent compositing over light, dark, and detailed footage.
7. Simulate disconnects, malformed payloads, timeouts, corrupt previews, and printer return.
8. Run a fixture-driven soak long enough to expose leaks or reconnect/thread growth.
9. Close OBS during MQTT, HTTP, and FTPS activity and confirm prompt clean shutdown.
10. Install the packaged artifact separately from the source tree and verify it while VS Code, terminals, and repository processes are closed.

If a live Bambu printer is available, validate MQTT, optional cloud metadata, FTPS preview fallback, job change, AMS active tray, and reconnect after printer restart. Never put live credentials or unsanitised payloads in commits or reports.

## Required documentation

Write user-facing documentation for:

- supported OBS versions, operating systems, and architectures;
- installer and manual ZIP installation;
- upgrade and uninstall;
- creating a connection profile;
- finding printer IP, serial, and LAN access code without publishing secrets;
- LAN-only versus optional cloud behavior;
- adding and styling the source;
- simulation mode;
- credential storage and TLS trust model;
- data freshness and reconnect behavior;
- logs and redacted diagnostics;
- migration from the Python/Flask version;
- known limitations;
- dependency licences and third-party attribution.

Write contributor documentation for reproducible local builds without implying that end users need those tools.

## Implementation workflow

1. Inspect the repository, git status/diff, current tests, and existing behavior.
2. Verify the current official OBS plugin template, API documentation, supported build environments, packaging layout, licensing, branding, and resource-submission policy.
3. Write a concise architecture decision record explaining why this is a native source and how networking, secrets, rendering, shared services, and migration work.
4. Establish deterministic protocol/state fixtures and parity tests before replacing behavior.
5. Integrate the official plugin template/CMake structure without discarding user changes.
6. Implement the connection/state layer and tests.
7. Implement native rendering and the three presets.
8. Implement plugin/source properties, profile settings, credentials, diagnostics, and simulation.
9. Validate lifecycle, performance, security, and visual output.
10. Package a clean Release artifact and test it outside the repository.
11. Update all documentation and produce a final evidence-based handoff.

Make reasonable decisions autonomously when they stay inside this scope. Surface a blocker only when it requires credentials, hardware, signing identity, legal choice, or a destructive product decision that cannot safely be inferred.

## Definition of done

The work is not done until all of the following are true:

- OBS lists **Bambu Printer Overlay** as a native source.
- One source replaces the current collection of text/image/SVG/browser sources.
- Live telemetry is rendered without Flask, localhost, file polling, or a Browser Source.
- The overlay remains responsive while the printer/network is slow or offline.
- Multiple sources share connections safely.
- Credentials are absent from scene exports, logs, diagnostics, tests, and release archives.
- Compact, Dashboard, and Expanded presets are visually complete and configurable.
- Existing important telemetry and preview fallback behavior have parity tests.
- Release builds and automated tests pass.
- The Windows installer/ZIP has the recommended OBS plugin structure.
- A packaged install works with the source tree, VS Code, terminals, Python, and development servers closed.
- OBS can shut down cleanly during active network operations.
- README installation instructions are suitable for a normal OBS user.
- The final report identifies changed files, architectural decisions, tests run, artifact paths, measurements, and any genuinely unverified live-hardware items.

## Authoritative references

Verify these before implementation and follow the current versions if they have changed:

- Official OBS plugin documentation: https://docs.obsproject.com/plugins
- Module API: https://docs.obsproject.com/reference-modules
- Source API: https://docs.obsproject.com/reference-sources
- Properties API: https://docs.obsproject.com/reference-properties
- Graphics API and context rules: https://docs.obsproject.com/graphics
- Official OBS plugin template: https://github.com/obsproject/obs-plugintemplate
- Official plugin installation/layout guide: https://obsproject.com/kb/plugins-guide
- Current OBS resource, licensing, branding, and AI disclosure policy: https://obsproject.com/forum/threads/forum-resource-and-ip-policy.178569/

When repository behavior and assumptions in this prompt differ, inspect the code and official current documentation, choose the safer supported behavior, implement it, and record the discrepancy.
