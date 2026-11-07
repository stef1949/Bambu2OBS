# Bambu2OBS

Bambu2OBS connects a Bambu Lab 3D printer to OBS Studio and displays live print progress, job metadata, temperatures, layers, speed, and preview artwork.

The live printer implementation is currently a Windows-friendly Python companion application. It connects to the printer over local MQTT and FTPS, optionally uses Bambu Cloud for job metadata, and serves a local OBS Browser Source.

An installable Windows x64 native preview is also built under [`native/`](native/). It loads directly in OBS without Python, Flask, localhost, or a Browser Source, but currently renders simulation data only. It is provided to validate installation and native OBS integration, not as a live-printer replacement yet.

## Current features

- Local MQTT telemetry with bounded reconnect backoff.
- Optional Bambu Cloud job name and preview retrieval.
- SD-card `.3mf` preview fallback with archive and image safety limits.
- Consolidated, transparent progress overlay with live/stale/offline status.
- Temperatures, layer count, remaining time, stage, speed, and print artwork.
- Multiple AMS units in the output data model, with legacy aliases for the first AMS.
- Atomic state writes so OBS and Flask do not read partially written files.
- TLS certificate pinning for self-signed printer services.

## Install the native OBS preview

The native preview requires 64-bit OBS Studio on Windows. It does not require Python or this repository at runtime.

1. Close OBS Studio.
2. Run `release\Bambu-Printer-Overlay-0.2.0-preview.1-Windows-x64-Setup.exe`.
3. Approve the administrator prompt. The installer writes to `C:\ProgramData\obs-studio\plugins`.
4. Start OBS Studio.
5. Select **Sources > + > Bambu Printer Overlay**.

The installer is an unsigned development build, so Windows may display a SmartScreen warning. Verify the adjacent `.sha256` file before running it. A portable ZIP with a hash-verifying `install.ps1` is also generated. See [native/PACKAGE_README.md](native/PACKAGE_README.md) for custom paths, upgrades, and uninstall instructions.

> The native preview's simulation warning is also displayed inside its OBS properties. Continue using the Python version below for live printer telemetry.

## Python-version requirements

- Windows 10 or 11.
- Python 3.11 or newer.
- OBS Studio.
- A Bambu printer with LAN access enabled.

Inkscape, VS Code, and Bambu Cloud are not required at runtime.

## Install the current Python version

Open PowerShell in the repository directory:

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
Copy-Item example.env .env
```

Edit `.env` and set at least:

```dotenv
PRINTER_SN=your_printer_serial_number
PRINTER_IP=192.0.2.10
ACCESS_CODE=your_access_code
BASE_DIR=data
```

`EMAIL`, `PASSWORD`, and `BAMBU_CLOUD_TOKEN` are optional. LAN telemetry continues to work when cloud authentication is unavailable.

## Choose a printer TLS trust mode

Bambu printer LAN services commonly use a self-signed certificate. Choose one of these modes explicitly.

### Preferred: pin the printer certificate

Set `PRINTER_TLS_FINGERPRINT_SHA256` to the 64-character SHA-256 fingerprint of the printer certificate. Colons and letter case are ignored.

The following PowerShell obtains the certificate presented by MQTT port 8883. Review that you are connected to the intended printer before trusting the value:

```powershell
$printerAddress = '192.0.2.10'
$tcpClient = [Net.Sockets.TcpClient]::new($printerAddress, 8883)
$acceptCertificate = { param($sender, $certificate, $chain, $errors) $true }
$tlsStream = [Net.Security.SslStream]::new($tcpClient.GetStream(), $false, $acceptCertificate)
$tlsStream.AuthenticateAsClient($printerAddress)
$certificateBytes = $tlsStream.RemoteCertificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert)
$sha256 = [Security.Cryptography.SHA256]::Create()
$fingerprint = [BitConverter]::ToString($sha256.ComputeHash($certificateBytes)).Replace('-', '')
$sha256.Dispose()
$fingerprint
$tlsStream.Dispose()
$tcpClient.Dispose()
```

Copy the result into `.env`:

```dotenv
PRINTER_TLS_FINGERPRINT_SHA256=0123456789ABCDEF...
ALLOW_INSECURE_PRINTER_TLS=false
```

### Compatibility exception

If pinning is not currently possible, the following explicit exception retains encryption but does not verify printer identity:

```dotenv
PRINTER_TLS_FINGERPRINT_SHA256=
ALLOW_INSECURE_PRINTER_TLS=true
```

Use this only on a trusted private network. See [SECURITY.md](SECURITY.md).

## Optional cloud metadata

For unattended use, prefer a token:

```dotenv
REGION=global
BAMBU_CLOUD_TOKEN=your_token
```

If the token is empty, the application can try `EMAIL` and `PASSWORD`. Accounts requiring an interactive verification code should use a token. Cloud credentials are never sent to third-party preview hosts.

## Run

With the virtual environment activated:

```powershell
python .\src\bambu2obs.py
```

The process starts the local overlay server, checks that it is listening, obtains initial metadata, and then enters the MQTT loop. Stop it with `Ctrl+C`.

The server listens only on `127.0.0.1`. Its port defaults to `5000` and can be changed with `PROGRESS_SERVER_PORT`.

## Add the overlay to OBS

Add a Browser Source using one of these local URLs:

- Consolidated overlay: `http://127.0.0.1:5000/view/progressbar`
- Print artwork only: `http://127.0.0.1:5000/view/printpreview`
- AMS overview: `http://127.0.0.1:5000/view/Filaments.svg`
- Active filament: `http://127.0.0.1:5000/view/ActiveFilament.svg`

Use a transparent Browser Source and choose dimensions appropriate for the scene; `760 × 180` is a practical starting point for the consolidated overlay.

The file at [`obs_template/Bambu2OBS_Scene.json`](obs_template/Bambu2OBS_Scene.json) is a legacy example containing machine-specific paths. It is not the recommended installation method and will be replaced by the native single-source workflow.

## Diagnostics

While the application is running:

- `http://127.0.0.1:5000/diagnostics/progress`
- `http://127.0.0.1:5000/diagnostics/writer`

The overlay marks data stale after 30 seconds by default. Change this with `OVERLAY_STALE_AFTER_SECONDS`.

`DEBUG_DUMPS=true` stores at most five diagnostic MQTT entries. Common credential fields are redacted, but always inspect diagnostics before sharing them.

## Test

```powershell
python -m pip install -r requirements-dev.txt
python -m compileall -q src tests
python -m unittest discover -s tests -v
python -m pip check
python -m ruff check src tests
```

CI runs the same checks on supported Python versions. Printer, cloud-account, and OBS integration still require proportional live testing because unit tests do not contain real credentials or hardware access.

The native build additionally runs an OBS-independent snapshot test and a headless libobs integration test that opens the DLL, initializes the module, registers the source, creates its properties, and unloads it.

## Native OBS plugin development

The native preview is kept separate from the working Python implementation until feature parity is demonstrated. Its CMake presets download only pinned, hash-verified OBS sources and development dependencies. The packaging script creates the recommended ProgramData-compatible layout, validates it, and emits an installer, portable ZIP, manifest, and SHA-256 sidecars.

See [`native/README.md`](native/README.md) and [`OVERLAY_OVERHAUL_PROMPT.md`](OVERLAY_OVERHAUL_PROMPT.md) for build instructions, current scope, and the remaining acceptance requirements.

Do not distribute a native build as production-ready until it has been built against a supported OBS version, loaded in OBS, tested during reconnect/shutdown, and validated with a real printer or sanitized protocol fixtures.

## Attribution and license

Bambu2OBS is based on the original work by [fluidman](https://github.com/fluidman). Local printer mappings use [Greg Hesp's pybambu](https://github.com/greghesp/pybambu), and optional cloud integration uses [coelacant1/Bambu-Lab-Cloud-API](https://github.com/coelacant1/Bambu-Lab-Cloud-API).

The existing repository is licensed under the MIT License; see [LICENSE](LICENSE). The native preview links against libobs and its package includes the exact GPL text from the pinned OBS source dependency. The licensing approach still requires owner review before a stable public release; the root licence has not been silently changed.

Direct dependency licence metadata and outstanding release-review requirements are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
