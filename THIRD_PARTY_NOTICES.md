# Third-party notices

## Native plugin

The plugin uses only OBS, the Qt that ships with OBS, and components of Windows. It ships no third-party binaries, fonts, icons, or images.

| Component | Use | Licence | Project |
| --- | --- | --- | --- |
| OBS Studio / libobs | Linked at runtime; built against 31.1.1 | GPL-2.0-or-later | <https://github.com/obsproject/obs-studio> |
| Qt 6 (Core, Gui, Widgets) | Settings dialog. Linked at runtime to the copy OBS ships; built against 6.8.3 | LGPL-3.0-only or GPL-2.0-only or GPL-3.0-only | <https://www.qt.io> |
| pybambu 1.0.1 | Source of the stage and filament identifier tables in `native/src/bambu-maps.c` | Apache-2.0 | <https://github.com/greghesp/pybambu> |

The inflate implementation in `native/src/bambu-zip.c` is original code following the structure of the DEFLATE specification (RFC 1951).

### Licensing of the distributed plugin

The repository's source code is MIT licensed. A distributed plugin binary links libobs, so that combined work is distributed under GPL-2.0-or-later, which is compatible with the MIT terms. Packages include the repository's MIT text, this notice, and the exact GPL text from the pinned OBS source. The complete corresponding source is this repository.

This reflects a reading of the licences and is not legal advice. Review it before publishing a release.

## Legacy Python application

These apply to `src/` only and are not included in any plugin package.

| Component | Version | Declared licence | Project |
| --- | --- | --- | --- |
| pybambu | 1.0.1 | Apache-2.0 | <https://github.com/greghesp/pybambu> |
| bambu-lab-cloud-api | 1.0.5 | AGPL-3.0 | <https://github.com/coelacant1/Bambu-Lab-Cloud-API> |
| Requests | 2.32.5 | Apache-2.0 | <https://requests.readthedocs.io> |
| Eclipse Paho MQTT | 2.1.0 | EPL-2.0 OR BSD-3-Clause | <https://eclipse.dev/paho/> |
| python-dotenv | 1.2.1 | BSD-3-Clause | <https://github.com/theskumar/python-dotenv> |
| Flask | 3.1.2 | BSD-3-Clause | <https://github.com/pallets/flask> |
