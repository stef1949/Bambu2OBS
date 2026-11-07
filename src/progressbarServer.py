from flask import Flask, jsonify, Response, send_from_directory, render_template_string, url_for
from werkzeug.utils import secure_filename, safe_join
import os
import time
import logging
import re
import json
from datetime import datetime
from dotenv import load_dotenv
from pathlib import Path

# Load environment variables
load_dotenv()


def env_int(name: str, default: int, minimum: int, maximum: int) -> int:
    raw_value = os.getenv(name)
    if raw_value in (None, ''):
        return default
    try:
        value = int(raw_value)
    except ValueError:
        return default
    return value if minimum <= value <= maximum else default

def resolve_base_dir():
    configured = os.getenv('BASE_DIR', 'data')
    repo_root = Path(__file__).resolve().parent.parent
    if os.path.isabs(configured):
        return configured

    repo_candidate = (repo_root / configured).resolve()
    if repo_candidate.is_dir():
        return str(repo_candidate)

    cwd_candidate = Path(os.path.abspath(configured))
    if cwd_candidate.is_dir():
        return str(cwd_candidate)

    return str(repo_candidate)

BASE_DIR = resolve_base_dir()
SVG_FILES = ('Filaments.svg', 'ActiveFilament.svg')
JOB_NAME_FILES = ['designTitle.txt', 'printProfile.txt']
COVER_IMAGE_NAME = 'printCover.png'
COVER_IMAGE_CANDIDATES = (COVER_IMAGE_NAME, 'printcover.png')
OVERLAY_REFRESH_MS = env_int('OVERLAY_REFRESH_MS', 5000, 1000, 60000)
OVERLAY_REQUEST_TIMEOUT_MS = env_int('OVERLAY_REQUEST_TIMEOUT_MS', 4000, 1000, 30000)
OVERLAY_STALE_AFTER_SECONDS = env_int('OVERLAY_STALE_AFTER_SECONDS', 30, 5, 3600)
PROGRESS_SERVER_PORT = env_int('PROGRESS_SERVER_PORT', 5000, 1024, 65535)
PLACEHOLDER_PREVIEW_SVG = """<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 300 400'>
<rect width='300' height='400' fill='#2f2f2f'/>
<line x1='150' y1='0' x2='150' y2='400' stroke='#0d0d0d' stroke-width='35'/>
<line x1='0' y1='280' x2='150' y2='220' stroke='#0d0d0d' stroke-width='35'/>
<line x1='150' y1='220' x2='300' y2='140' stroke='#0d0d0d' stroke-width='35'/>
</svg>"""

app = Flask(__name__)

# Configure logging
app.logger.setLevel(logging.DEBUG)

PROGRESS_FILE_PATH = os.path.join(BASE_DIR, 'progress.txt')
PROGRESS_FILE_FALLBACK = os.path.join(BASE_DIR, 'progressPercent.txt')
WRITER_STATUS_FILE_PATH = os.path.join(BASE_DIR, 'writerStatus.json')
PROGRESS_NUMBER_RE = re.compile(r'[-+]?\d*\.?\d+')
SVG_DIR = BASE_DIR  # Assuming SVG files are stored in the BASE_DIR
TELEMETRY_FILES = {
    'remaining_time': 'remaining_time.txt',
    'layer': 'layerOverview.txt',
    'bed_temperature': 'bedTemperature.txt',
    'nozzle_temperature': 'nozzleTemperature.txt',
    'print_speed': 'printSpeed.txt',
    'print_stage': 'printStage.txt',
    'fan_speed': 'coolingFanSpeed.txt'
}

def known_svg_file(filename):
    """Return the matching entry of SVG_FILES, or None.

    The result is the server's own string, never the request's, so it is safe
    to use in paths and pages.
    """
    for name in SVG_FILES:
        if filename == name:
            return name
    return None


def no_store(response):
    response = app.make_response(response)
    response.headers['Cache-Control'] = 'no-store, no-cache, must-revalidate, max-age=0'
    response.headers['Pragma'] = 'no-cache'
    response.headers['Expires'] = '0'
    return response


def detect_file_image_mime(path):
    try:
        with open(path, 'rb') as image_file:
            signature = image_file.read(12)
    except OSError:
        return None
    if signature.startswith(b'\x89PNG\r\n\x1a\n'):
        return 'image/png'
    if signature.startswith(b'\xff\xd8\xff'):
        return 'image/jpeg'
    if len(signature) >= 12 and signature[:4] == b'RIFF' and signature[8:12] == b'WEBP':
        return 'image/webp'
    return None


def read_writer_runtime():
    payload = {
        'state': 'configuration_required',
        'display_state': 'offline',
        'stale': True,
        'age_seconds': None
    }
    try:
        stat = os.stat(WRITER_STATUS_FILE_PATH)
        age_seconds = max(0.0, round(time.time() - stat.st_mtime, 3))
        with open(WRITER_STATUS_FILE_PATH, 'r', encoding='utf-8') as status_file:
            status = json.load(status_file)
    except (OSError, ValueError):
        return payload

    state = str(status.get('state') or 'unknown')
    stale = age_seconds > OVERLAY_STALE_AFTER_SECONDS
    if state in {'mqtt_connected', 'mqtt_loop_running', 'mqtt_message_received'} and not stale:
        display_state = 'live'
    elif state in {'mqtt_connecting', 'starting', 'initialized', 'progress_server_started'}:
        display_state = 'connecting'
    elif state in {'mqtt_connect_failed', 'mqtt_disconnected'}:
        display_state = 'reconnecting'
    elif state in {'cloud_unavailable'} and not stale:
        display_state = 'lan_only'
    elif state == 'error':
        display_state = 'error'
    else:
        display_state = 'stale' if stale else 'offline'

    payload.update({
        'state': state,
        'display_state': display_state,
        'stale': stale,
        'age_seconds': age_seconds,
        'updated_at': status.get('updated_at')
    })
    return payload


def read_overlay_telemetry():
    telemetry = {}
    for key, filename in TELEMETRY_FILES.items():
        telemetry[key] = read_first_nonempty_text([filename])
    return telemetry

def read_first_nonempty_text(file_candidates):
    """Return the first non-empty text value from the provided filenames."""
    for filename in file_candidates:
        path = os.path.join(BASE_DIR, filename)
        try:
            with open(path, 'r', encoding='utf-8') as file:
                content = file.read().strip()
                if content:
                    return content
        except FileNotFoundError:
            continue
    return None

def _parse_progress_value(raw_value):
    if raw_value is None:
        return None
    if isinstance(raw_value, (int, float)):
        return float(raw_value)
    text = str(raw_value).strip()
    if not text:
        return None
    match = PROGRESS_NUMBER_RE.search(text)
    if not match:
        return None
    try:
        return float(match.group(0))
    except ValueError:
        return None

def read_progress_value():
    details = read_progress_details()
    return details['progress']

def read_progress_details():
    """Return parsed progress plus file-level diagnostics for troubleshooting."""
    file_details = []
    selected_progress = None
    selected_source_path = None

    for path in (PROGRESS_FILE_PATH, PROGRESS_FILE_FALLBACK):
        detail = {
            'path': path,
            'filename': os.path.basename(path),
            'exists': os.path.exists(path),
            'parsed_progress': None
        }
        file_details.append(detail)

        if not detail['exists']:
            continue

        try:
            stat = os.stat(path)
            detail['size_bytes'] = stat.st_size
            detail['mtime'] = datetime.fromtimestamp(stat.st_mtime).isoformat()
            detail['age_seconds'] = max(0.0, round(time.time() - stat.st_mtime, 3))
            with open(path, 'r', encoding='utf-8') as file:
                raw_value = file.read()
                trimmed = raw_value.strip()
                detail['raw'] = trimmed[:64]
                value = _parse_progress_value(raw_value)
                detail['parsed_progress'] = value
                if value is None:
                    detail['error'] = "No numeric value found"
                elif selected_progress is None:
                    selected_progress = value
                    selected_source_path = path
        except OSError:
            app.logger.warning('Could not read %s', path, exc_info=True)
            detail['error'] = 'Could not read the file. See the server log for details.'
            continue

    return {
        'progress': selected_progress,
        'source_path': selected_source_path,
        'files': file_details
    }

def file_watcher(filename, last_known_stamp=0):
    """
    Generator function to watch for file changes.
    """
    while True:
        try:
            stat = os.stat(os.path.join(SVG_DIR, filename))
            if stat.st_mtime != last_known_stamp:
                last_known_stamp = stat.st_mtime
                yield "data: update\n\n"
        except FileNotFoundError:
            pass
        time.sleep(10)

def get_job_name():
    """Retrieve the most relevant job name available on disk."""
    job_name = read_first_nonempty_text(JOB_NAME_FILES)
    return job_name or "Unknown Print"

def cover_image_name():
    """Return the available cover image filename, if any."""
    for candidate in COVER_IMAGE_CANDIDATES:
        candidate_path = os.path.join(BASE_DIR, candidate)
        if os.path.exists(candidate_path):
            return candidate
    return None

def cover_image_version(filename):
    """Return a stable cache-busting token for the current cover image."""
    if not filename:
        return None
    try:
        stat = os.stat(os.path.join(BASE_DIR, filename))
    except OSError:
        return None
    return f"{stat.st_mtime_ns}-{stat.st_size}"

@app.route('/job-info')
def job_info():
    cover_name = cover_image_name()
    cover_available = cover_name is not None
    payload = {
        'job_name': get_job_name(),
        'has_cover': cover_available,
        'cover_url': url_for('serve_print_cover') if cover_available else None,
        'cover_version': cover_image_version(cover_name) if cover_available else None,
        'preview_image_url': url_for('print_preview_image'),
        'runtime': read_writer_runtime(),
        'telemetry': read_overlay_telemetry()
    }
    return no_store(jsonify(payload))

@app.route('/print-cover')
def serve_print_cover():
    cover_name = cover_image_name()
    if cover_name:
        response = send_from_directory(BASE_DIR, cover_name)
        image_mime = detect_file_image_mime(os.path.join(BASE_DIR, cover_name))
        if image_mime:
            response.mimetype = image_mime
        return no_store(response)
    return no_store(("File not found", 404))

@app.route('/print-preview-image')
def print_preview_image():
    cover_name = cover_image_name()
    if cover_name:
        response = send_from_directory(BASE_DIR, cover_name)
        image_mime = detect_file_image_mime(os.path.join(BASE_DIR, cover_name))
        if image_mime:
            response.mimetype = image_mime
    else:
        response = Response(PLACEHOLDER_PREVIEW_SVG, mimetype='image/svg+xml')
    return no_store(response)

@app.route('/progress')
def get_progress():
    try:
        progress_details = read_progress_details()
        progress_value = progress_details['progress']
        source_path = progress_details['source_path']
        if progress_value is None:
            response = jsonify({
                'progress': None,
                'source': None,
                'error': "No readable progress value found in progress.txt or progressPercent.txt"
            })
        else:
            source_name = os.path.basename(source_path) if source_path else None
            response = jsonify({'progress': progress_value, 'source': source_name})
            if source_name:
                response.headers['X-Progress-Source'] = source_name
        return no_store(response)
    except Exception:
        app.logger.exception('Failed to read progress')
        return no_store((jsonify({'error': 'Failed to read progress. See the server log for details.'}), 500))

@app.route('/diagnostics/progress')
def diagnostics_progress():
    progress_details = read_progress_details()
    source_path = progress_details['source_path']
    response = jsonify({
        'base_dir': BASE_DIR,
        'cwd': os.getcwd(),
        'checked_at': datetime.utcnow().isoformat() + 'Z',
        'selected_progress': progress_details['progress'],
        'selected_source': os.path.basename(source_path) if source_path else None,
        'files': progress_details['files']
    })
    return no_store(response)

@app.route('/diagnostics/writer')
def diagnostics_writer():
    payload = {
        'base_dir': BASE_DIR,
        'cwd': os.getcwd(),
        'checked_at': datetime.utcnow().isoformat() + 'Z',
        'status_file': WRITER_STATUS_FILE_PATH,
        'status_file_exists': os.path.exists(WRITER_STATUS_FILE_PATH),
        'writer_status': None
    }

    if payload['status_file_exists']:
        try:
            stat = os.stat(WRITER_STATUS_FILE_PATH)
            payload['status_file_mtime'] = datetime.fromtimestamp(stat.st_mtime).isoformat()
            payload['status_file_age_seconds'] = max(0.0, round(time.time() - stat.st_mtime, 3))
            with open(WRITER_STATUS_FILE_PATH, 'r', encoding='utf-8') as status_file:
                payload['writer_status'] = json.load(status_file)
        except OSError:
            app.logger.warning('Could not read %s', WRITER_STATUS_FILE_PATH, exc_info=True)
            payload['error'] = 'writerStatus.json could not be read. See the server log for details.'
        except ValueError:
            app.logger.warning('Could not parse %s', WRITER_STATUS_FILE_PATH, exc_info=True)
            payload['error'] = 'writerStatus.json is not valid JSON. See the server log for details.'
    else:
        payload['error'] = 'writerStatus.json not found. Start bambu2obs.py to populate writer diagnostics.'

    response = jsonify(payload)
    return no_store(response)

@app.route('/view/printpreview')
def printpreview_view():
    html = """
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <title>Print Preview</title>
        <style>
            html, body {
                margin: 0;
                padding: 0;
                width: 100%;
                height: 100%;
                overflow: hidden;
                background: transparent;
            }
            .preview-wrapper {
                width: 100%;
                height: 100%;
                display: flex;
                align-items: center;
                justify-content: center;
            }
            .preview-image {
                width: 100%;
                height: 100%;
                object-fit: contain;
            }
        </style>
    </head>
    <body>
        <div class="preview-wrapper">
            <img id="print-preview" class="preview-image" alt="Print preview image">
        </div>
        <script>
            const previewImage = document.getElementById('print-preview');
            const REFRESH_MS = Math.max(1000, Number({{ refresh_ms }}));
            const baseUrl = (window.location.origin && window.location.origin !== 'null')
                ? window.location.origin
                : 'http://localhost:5000';

            function buildUrl(path) {
                const url = new URL(path, baseUrl);
                url.searchParams.set('t', Date.now());
                return url.toString();
            }

            function refreshPreview() {
                previewImage.src = buildUrl('/print-preview-image');
            }

            refreshPreview();
            setInterval(refreshPreview, REFRESH_MS);
        </script>
    </body>
    </html>
    """
    return no_store(render_template_string(html, refresh_ms=OVERLAY_REFRESH_MS))

@app.route('/view/progressbar')
def progressbar_view():
    html = """
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <title>Bambu2OBS Printer Overlay</title>
        <style>
            html, body {
                margin: 0;
                padding: 0;
                overflow: hidden;
            }
            .overlay-wrapper {
                box-sizing: border-box;
                padding: 14px;
                max-width: 760px;
                margin: 0 auto;
                display: flex;
                flex-direction: column;
                gap: 10px;
                font-family: "Segoe UI", Tahoma, sans-serif;
                color: #f5f5f5;
                background: linear-gradient(135deg, rgba(15, 18, 22, 0.92), rgba(26, 31, 37, 0.84));
                border: 1px solid rgba(255, 255, 255, 0.12);
                border-radius: 14px;
                box-shadow: 0 12px 32px rgba(0, 0, 0, 0.28);
            }
            .job-info {
                display: flex;
                align-items: center;
            }
            .job-cover {
                width: 128px;
                height: 128px;
                border-radius: 8px;
                object-fit: contain;
                margin-right: 14px;
                background-color: transparent;
                display: none;
                /* filter: invert(100%); */
            }
            .job-details {
                flex: 1;
                min-width: 0;
            }
            .job-heading {
                display: flex;
                align-items: center;
                justify-content: space-between;
                gap: 10px;
            }
            .job-cover.visible {
                display: block;
            }
            .job-name {
                font-size: 1.1rem;
                font-weight: 600;
                margin-bottom: 8px;
                white-space: nowrap;
                overflow: hidden;
                text-overflow: ellipsis;
            }
            .status-pill {
                flex: none;
                padding: 3px 8px;
                border-radius: 999px;
                font-size: 0.7rem;
                font-weight: 700;
                letter-spacing: 0.04em;
                text-transform: uppercase;
                background: rgba(148, 163, 184, 0.2);
                color: #d7dee8;
            }
            .status-pill.live { background: rgba(0, 174, 66, 0.22); color: #72f3a3; }
            .status-pill.connecting, .status-pill.reconnecting { background: rgba(245, 158, 11, 0.22); color: #fbd274; }
            .status-pill.error, .status-pill.offline { background: rgba(239, 68, 68, 0.22); color: #fca5a5; }
            .status-pill.stale { background: rgba(148, 163, 184, 0.22); color: #d7dee8; }
            .progress-shell {
                position: relative;
            }
            .progress {
                position: relative;
                overflow: hidden;
                border-radius: 999px;
                background-color: rgba(238, 238, 238, 0.24);
                height: 30px;
                margin: 0;
                width: 100%;
            }
            .progress-bar {
                height: 100%;
                background-color: #00AE42;
                transition: width 400ms ease;
            }
            .progress-value {
                position: absolute;
                inset: 0;
                display: grid;
                place-items: center;
                font-size: 0.8rem;
                font-weight: 700;
                text-shadow: 0 1px 2px rgba(0, 0, 0, 0.75);
                pointer-events: none;
            }
            .telemetry-grid {
                display: grid;
                grid-template-columns: repeat(3, minmax(0, 1fr));
                gap: 6px 12px;
                margin-top: 10px;
            }
            .telemetry-item {
                min-width: 0;
                font-size: 0.75rem;
                color: #cbd5e1;
                white-space: nowrap;
                overflow: hidden;
                text-overflow: ellipsis;
            }
            .telemetry-item strong {
                color: #ffffff;
                font-weight: 600;
            }
            @media (max-width: 520px) {
                .job-cover { width: 88px; height: 88px; }
                .telemetry-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
            }
            @media (prefers-reduced-motion: reduce) {
                .progress-bar { transition: none; }
            }
        </style>
    </head>
    <body>

    <div class="overlay-wrapper">
        <div class="job-info">
            <img id="job-cover" class="job-cover" alt="Print cover preview">
            <div class="job-details">
                <div class="job-heading">
                    <div id="job-name" class="job-name">Loading print...</div>
                    <div id="status-pill" class="status-pill">Connecting</div>
                </div>
                <div class="progress-shell">
                    <div class="progress">
                        <div id="progress-bar" class="progress-bar" role="progressbar" style="width: 0%;" aria-valuenow="0" aria-valuemin="0" aria-valuemax="100"></div>
                    </div>
                    <div id="progress-value" class="progress-value">0%</div>
                </div>
                <div class="telemetry-grid">
                    <div id="remaining-time" class="telemetry-item"></div>
                    <div id="layer" class="telemetry-item"></div>
                    <div id="print-stage" class="telemetry-item"></div>
                    <div id="nozzle-temperature" class="telemetry-item"></div>
                    <div id="bed-temperature" class="telemetry-item"></div>
                    <div id="print-speed" class="telemetry-item"></div>
                </div>
            </div>
        </div>
    </div>

    <script>
        const PLACEHOLDER_COVER = "data:image/svg+xml;charset=UTF-8,%3Csvg%20xmlns%3D%27http%3A//www.w3.org/2000/svg%27%20viewBox%3D%270%200%20300%20400%27%3E%3Crect%20width%3D%27300%27%20height%3D%27400%27%20fill%3D%27%232f2f2f%27/%3E%3Cline%20x1%3D%27150%27%20y1%3D%270%27%20x2%3D%27150%27%20y2%3D%27400%27%20stroke%3D%27%230d0d0d%27%20stroke-width%3D%2735%27/%3E%3Cline%20x1%3D%270%27%20y1%3D%27280%27%20x2%3D%27150%27%20y2%3D%27220%27%20stroke%3D%27%230d0d0d%27%20stroke-width%3D%2735%27/%3E%3Cline%20x1%3D%27150%27%20y1%3D%27220%27%20x2%3D%27300%27%20y2%3D%27140%27%20stroke%3D%27%230d0d0d%27%20stroke-width%3D%2735%27/%3E%3C/svg%3E";
        const jobCover = document.getElementById('job-cover');
        const jobName = document.getElementById('job-name');
        const statusPill = document.getElementById('status-pill');
        const progressBar = document.getElementById('progress-bar');
        const progressValue = document.getElementById('progress-value');
        const REFRESH_MS = Math.max(1000, Number({{ refresh_ms }}));
        const REQUEST_TIMEOUT_MS = Math.max(1000, Number({{ request_timeout_ms }}));
        const baseUrl = (window.location.origin && window.location.origin !== 'null')
            ? window.location.origin
            : 'http://localhost:5000';
        let refreshTimer = null;
        let refreshWatchdog = null;
        let refreshInFlight = false;
        let lastProgressValue = null;
        let lastCoverSrc = '';

        function buildUrl(path) {
            const url = new URL(path, baseUrl);
            url.searchParams.set('t', Date.now());
            return url.toString();
        }

        function fetchJson(path) {
            const controller = (typeof AbortController !== 'undefined')
                ? new AbortController()
                : null;
            const timeoutId = controller
                ? window.setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS)
                : null;
            const options = {
                cache: 'no-store',
                headers: {
                    'Cache-Control': 'no-cache',
                    'Pragma': 'no-cache'
                }
            };

            if (controller) {
                options.signal = controller.signal;
            }

            return fetch(buildUrl(path), options)
                .then(response => {
                    if (!response.ok) {
                        throw new Error(`HTTP ${response.status}`);
                    }
                    return response.json();
                })
                .finally(() => {
                    if (timeoutId) {
                        window.clearTimeout(timeoutId);
                    }
                });
        }

        function showPlaceholderCover() {
            jobCover.src = PLACEHOLDER_COVER;
            jobCover.classList.add('visible');
            lastCoverSrc = PLACEHOLDER_COVER;
        }

        jobCover.addEventListener('error', () => {
            showPlaceholderCover();
        }, { once: false });

        function applyProgress(value) {
            const numericValue = parseFloat(value);
            if (!Number.isFinite(numericValue)) {
                return;
            }

            const clampedValue = Math.min(100, Math.max(0, numericValue));
            if (clampedValue === lastProgressValue) {
                return;
            }

            lastProgressValue = clampedValue;
            window.requestAnimationFrame(() => {
                progressBar.style.width = `${clampedValue}%`;
                progressBar.setAttribute('aria-valuenow', clampedValue);
                progressValue.textContent = `${Math.round(clampedValue)}%`;
            });
        }

        function applyTelemetry(telemetry = {}) {
            const values = [
                ['remaining-time', 'ETA', telemetry.remaining_time, ''],
                ['layer', '', telemetry.layer, ''],
                ['print-stage', 'Stage', telemetry.print_stage, ''],
                ['nozzle-temperature', 'Nozzle', telemetry.nozzle_temperature, '°C'],
                ['bed-temperature', 'Bed', telemetry.bed_temperature, '°C'],
                ['print-speed', 'Speed', telemetry.print_speed, '']
            ];
            values.forEach(([id, label, value, suffix]) => {
                const element = document.getElementById(id);
                if (!value) {
                    element.textContent = '';
                    element.style.display = 'none';
                    return;
                }
                element.style.display = 'block';
                element.textContent = label ? `${label}: ${value}${suffix}` : `${value}${suffix}`;
            });
        }

        function applyRuntime(runtime = {}) {
            const displayState = runtime.display_state || 'offline';
            statusPill.className = `status-pill ${displayState}`;
            statusPill.textContent = displayState.replace('_', ' ');
        }

        function updateProgress() {
            return fetchJson('/progress')
                .then(data => {
                    applyProgress(data.progress);
                })
                .catch(error => console.error('Error fetching progress:', error));
        }

        function updateJobInfo() {
            return fetchJson('/job-info')
                .then(data => {
                    jobName.textContent = data.job_name || 'Unknown Print';
                    applyRuntime(data.runtime);
                    applyTelemetry(data.telemetry);
                    if (data.has_cover && data.cover_url) {
                        const coverUrl = new URL(data.cover_url, baseUrl);
                        coverUrl.searchParams.set('v', data.cover_version || Date.now());
                        const nextCoverSrc = coverUrl.toString();
                        if (nextCoverSrc !== lastCoverSrc) {
                            jobCover.src = nextCoverSrc;
                            lastCoverSrc = nextCoverSrc;
                        }
                        jobCover.classList.add('visible');
                    } else {
                        showPlaceholderCover();
                    }
                })
                .catch(error => console.error('Error fetching job info:', error));
        }

        function refreshOverlay() {
            if (refreshInFlight) {
                return;
            }

            refreshInFlight = true;
            if (refreshWatchdog) {
                window.clearTimeout(refreshWatchdog);
            }
            refreshWatchdog = window.setTimeout(() => {
                refreshInFlight = false;
                scheduleOverlayRefresh();
            }, REQUEST_TIMEOUT_MS + 1000);

            Promise.all([updateProgress(), updateJobInfo()])
                .finally(() => {
                    if (refreshWatchdog) {
                        window.clearTimeout(refreshWatchdog);
                        refreshWatchdog = null;
                    }
                    refreshInFlight = false;
                    scheduleOverlayRefresh();
                });
        }

        function scheduleOverlayRefresh(delay = REFRESH_MS) {
            if (refreshTimer) {
                window.clearTimeout(refreshTimer);
            }
            refreshTimer = window.setTimeout(refreshOverlay, delay);
        }

        function restartOverlayRefresh() {
            scheduleOverlayRefresh(0);
        }

        document.addEventListener('visibilitychange', () => {
            if (!document.hidden) {
                restartOverlayRefresh();
            }
        });
        window.addEventListener('focus', restartOverlayRefresh);
        window.addEventListener('pageshow', restartOverlayRefresh);
        restartOverlayRefresh();
    </script>

    </body>
    </html>
    """
    return no_store(render_template_string(
        html,
        refresh_ms=OVERLAY_REFRESH_MS,
        request_timeout_ms=OVERLAY_REQUEST_TIMEOUT_MS
    ))

@app.route('/updates/<filename>')
def updates(filename):
    name = known_svg_file(filename)
    if name:
        response = Response(file_watcher(name), content_type='text/event-stream')
        response.headers['X-Accel-Buffering'] = 'no'
        return no_store(response)
    return no_store(("File not found", 404))

@app.route('/svg/<filename>')
def serve_svg(filename):
    filename = secure_filename(filename)
    filepath = safe_join(SVG_DIR, filename)
    if filepath and os.path.isfile(filepath):
        return no_store(send_from_directory(SVG_DIR, filename))
    else:
        app.logger.info("SVG file not found: %s", filename)
        return no_store(("File not found", 404))

@app.route('/view/<filename>')
def view_svg(filename):
    name = known_svg_file(filename)
    if not name:
        return no_store(("File not found", 404))
    html = """
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <title>SVG Viewer - {{ name }}</title>
    </head>
    <body>
        <img src="{{ svg_url }}" id="svgImage">
        <script>
            const evtSource = new EventSource({{ updates_url|tojson }});
            evtSource.onmessage = function(event) {
                const img = document.getElementById('svgImage');
                const src = img.src.split('?')[0];
                img.src = `${src}?t=${new Date().getTime()}`;
            };
        </script>
    </body>
    </html>
    """
    return no_store(render_template_string(
        html,
        name=name,
        svg_url=url_for('serve_svg', filename=name),
        updates_url=url_for('updates', filename=name)
    ))

if __name__ == '__main__':
    # Disable the Flask reloader to avoid recursive subprocess spawning when
    # this script is launched from bambu2obs.py.
    app.run(
        debug=False,
        use_reloader=False,
        host='127.0.0.1',
        port=PROGRESS_SERVER_PORT,
        threaded=True
    )
