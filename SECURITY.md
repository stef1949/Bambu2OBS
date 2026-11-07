# Security policy

## Reporting a vulnerability

Do not open a public issue containing printer access codes, cloud tokens, account details, certificate fingerprints, or unredacted MQTT payloads. Contact the repository owner privately first and include only the minimum reproduction information required.

## Credential handling

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

## Native preview installer

Preview installers are currently unsigned. Verify the adjacent SHA-256 sidecar before execution and download artifacts only from a trusted build or release. The portable installer verifies every plugin payload file against `manifest.json`, refuses ProgramData installation without elevation, and refuses to modify plugin files while OBS is running.

The native preview contains no credentials and does not yet connect to a printer. Do not infer live-printer security parity from the installable simulation build.
