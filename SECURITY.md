# Security policy

## Reporting a vulnerability

Do not open a public issue containing printer access codes, cloud tokens, account details, certificate fingerprints, or unredacted MQTT payloads. Contact the repository owner privately first and include only the minimum reproduction information required.

## Plugin credentials

- Access codes and cloud tokens are stored in Windows Credential Manager, per profile. They are never written to scene collections, profile files, logs, or diagnostics, and stored secrets are never read back into the settings dialog.
- `profiles.json` in the OBS configuration folder holds only non-secret settings and certificate fingerprints.
- Log messages pass through redaction of bearer tokens, email addresses, and URL query strings. Diagnostics shorten the serial number to its last four characters.

## Plugin TLS trust

Printer connections pin a SHA-256 certificate fingerprint that the user approves. **Test Connection** reads the certificate without sending credentials, and the access code is sent only after approval and only over a connection matching the pin. A profile with no pin does not connect. A mismatch stops the connection with a visible error, and changing a profile's host clears its pins.

Approving a certificate is trust on first use: it is only as good as the network at that moment, so do it on a network you trust. Pinning covers MQTT and FTPS. Cloud HTTPS keeps full default validation, refuses redirects, and sends the token only to the Bambu API host.

Untrusted input is bounded: MQTT packets to 1 MiB, JSON depth and size, `.3mf` archives to 256 MiB and 4096 members, preview images to 10 MiB and 8192 pixels per side.

## Legacy Python credential handling

- Keep `.env` out of source control. It is ignored by this repository.
- Prefer a Bambu Cloud token to an account password when cloud metadata is enabled.
- Never attach `ConnectionDumps.json`, `writerStatus.json`, or OBS logs without reviewing them for identifying information.
- Rotate an access code or token immediately if it is accidentally committed or published.

## Printer TLS trust

The legacy Python client supports three trust modes:

1. `PRINTER_TLS_FINGERPRINT_SHA256` pins the printer certificate and is preferred for the self-signed certificates commonly used by printer LAN services.
2. With neither TLS variable set, the operating-system CA store and hostname validation are used.
3. `ALLOW_INSECURE_PRINTER_TLS=true` is an explicit compatibility exception. It encrypts traffic but does not verify printer identity.

Never use the insecure exception on an untrusted or shared network. TLS exceptions apply only to the printer MQTT and FTPS connections; cloud HTTPS verification remains enabled.

## Network exposure

The legacy overlay server binds only to `127.0.0.1`. Global CORS is intentionally disabled. Do not expose the Flask development server to another computer or the public internet.

## Download limits

Cloud images and printer `.3mf` archives are bounded before decoding. Keep these limits finite and increase them only when a known legitimate job requires it.

## Installers

Installers are currently unsigned. Verify the adjacent SHA-256 sidecar before execution and download artifacts only from a trusted build or release. The portable installer verifies every plugin payload file against `manifest.json`, refuses ProgramData installation without elevation, and refuses to modify plugin files while OBS is running.

Packages contain no credentials.
