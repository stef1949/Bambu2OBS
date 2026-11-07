# Troubleshooting

Start with the status badge on the overlay and the status line at the top of the source's properties.

| Badge | Meaning | What to do |
| --- | --- | --- |
| **SETUP NEEDED** | No profile chosen, or the profile is incomplete. | Choose a profile in the source's properties, or open **Tools > Bambu Printer Overlay Settings**. Enter the access code and run **Test Connection**. |
| **CONNECTING** | The first connection attempt is running. | Wait a few seconds. |
| **LIVE** | Connected and receiving data. | Nothing. |
| **STALE** | Connected, but nothing has arrived recently. | The overlay keeps showing the last known values. Check the printer's network connection. The threshold is under **Formatting**. |
| **RECONNECTING** | The connection dropped and is being retried. | Usually clears on its own. |
| **OFFLINE** | Several attempts failed. It keeps retrying, up to once every two minutes. | Check the printer is on and its IP address has not changed. |
| **AUTH FAILED** | The printer rejected the access code. | The code changes if LAN mode is toggled or the printer is reset. Enter the current one and run **Test Connection**. |
| **ERROR** | Usually a changed certificate. | See below. |

## The source is missing from the Add Source menu

1. Open **Help > Log Files > View Current Log** and search for `bambu-printer-overlay`.
2. Confirm OBS is 64-bit, version 33 or newer, and the DLL exists at `C:\ProgramData\obs-studio\plugins\bambu-printer-overlay\bambu-printer-overlay.dll`.
3. Restart OBS after installing.

## Test Connection says the printer can't be reached

- Check the IP on the printer's network screen. It can change, so a DHCP reservation helps.
- The printer and PC must be on the same network, and client isolation or guest Wi-Fi often blocks this.
- Port 8883 must be reachable. Some firewalls and VPNs block it.

## "Printer certificate changed"

The certificate no longer matches the one you trusted, so the plugin stopped rather than sending your access code to it. This is expected after a factory reset, a motherboard replacement, or some firmware updates. If none of those happened, make sure the address really is your printer. To accept the new certificate, run **Test Connection** and approve the prompt.

## No print preview

The overlay shows a placeholder until a preview is available. Previews come from Bambu Cloud when a token is configured and LAN-only is off, and otherwise from the `.3mf` on the printer over FTPS.

- **Fetch print previews** must be on in the profile.
- FTPS needs its own trusted certificate. Run **Test Connection** once.
- Export diagnostics and read the `preview:` line for the reason.
- The plugin stops after six failed attempts for a job. **Clear Preview** in the settings retries immediately.

## Everything shows a dash

A dash means the printer has not reported that value. Some values only appear during a print, and total time and weight need cloud metadata.

## The preview or AMS disappeared, or text is hard to read on stream

The overlay removes detail rather than letting text shrink until it can't be read, so a small source shows less:

| Source width | Shows |
| --- | --- |
| 816 or more (Dashboard and Expanded) | The full preset |
| 544 to 815 | Compact: percent, job, progress, time left, and status |
| Under 544 | Percent, job, and progress. A problem such as **OFFLINE** replaces the job name |

Height counts too: a source below 85% of its preset height steps down in the same way. Increase **Width** and **Height** in the source's properties to bring the detail back.

With many AMS trays, chips show a short material name, then only the colour.

If text looks soft on stream, check that the source isn't scaled down in the scene: right-click it and choose **Transform > Edit Transform**. See [Keep it readable on stream](../README.md#keep-it-readable-on-stream).

## Reporting a problem

Use **Export redacted diagnostics**. It writes a text report with the serial number shortened and no access code or token. Read it before sharing, and search the OBS log for `bambu-printer-overlay`.

Never post an access code, cloud token, or raw MQTT payload.
