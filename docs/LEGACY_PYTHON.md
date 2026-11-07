# Legacy Python companion

This is the original Bambu2OBS implementation: a Python process that writes state to files and serves a Flask overlay to an OBS Browser Source. It has been superseded by the native **Bambu Printer Overlay** plugin, which needs none of it. See the [main README](../README.md) and the [migration guide](MIGRATION.md).

It stays in the repository, unchanged, until the native plugin has been validated against real printers. It is excluded from every plugin package.

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

Use this only on a trusted private network. See [SECURITY.md](../SECURITY.md).

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

The file at [`obs_template/Bambu2OBS_Scene.json`](../obs_template/Bambu2OBS_Scene.json) is an example containing machine-specific paths. The native plugin replaces it with a single source.

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
