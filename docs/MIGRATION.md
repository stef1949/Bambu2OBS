# Migrating from the Python version

The plugin replaces the Python process, the Flask server, the generated text and image files, and the collection of Browser, text, and image sources. One source now does all of that.

## Steps

1. Stop the Python app (`Ctrl+C`).
2. [Install the plugin](../README.md#installation).
3. In OBS open **Tools > Bambu Printer Overlay Settings** and add a printer, using these values from your `.env`:

   | `.env` | Settings field |
   | --- | --- |
   | `PRINTER_IP` | Printer IP or host |
   | `PRINTER_SN` | Serial number |
   | `ACCESS_CODE` | LAN access code |
   | `BAMBU_CLOUD_TOKEN` | Cloud token (optional; also untick **LAN only**) |
   | `REGION=china` | China region |

4. Press **Save**, then **Test Connection**, and approve the certificate prompt.
5. Add a **Bambu Printer Overlay** source and choose the profile.
6. Delete the old Browser, text, and image sources from your scene.

## What no longer applies

- `EMAIL` and `PASSWORD`: use a cloud token instead.
- `ALLOW_INSECURE_PRINTER_TLS`: the plugin always pins the certificate, and asks you to approve it once.
- `PRINTER_TLS_FINGERPRINT_SHA256`: replaced by the approval step, which stores the fingerprint for you.
- `obs_template/Bambu2OBS_Scene.json`: not needed.
- Python, pip, the virtual environment, and `localhost:5000`.

## Removing the old setup

Once the overlay works, delete your virtual environment and the generated `data` folder. Then delete `.env`, since it holds your access code in plain text and the plugin keeps that in Windows Credential Manager instead.

The Python code remains in the repository for now, documented in [LEGACY_PYTHON.md](LEGACY_PYTHON.md).

## From the 0.2 preview

Scenes containing the preview's source keep working: its source ID and settings keys are unchanged. Two things differ. Simulation is now off by default, so a source with no profile shows **SETUP NEEDED**, and a size you set is kept.
