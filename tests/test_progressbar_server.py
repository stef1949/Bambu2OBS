import importlib
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.parse import quote


REPO_ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = REPO_ROOT / "src"

if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))


class ProgressbarServerTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.previous_base_dir = os.environ.get("BASE_DIR")
        os.environ["BASE_DIR"] = self.temp_dir.name

        if "progressbarServer" in sys.modules:
            del sys.modules["progressbarServer"]

        self.module = importlib.import_module("progressbarServer")
        self.client = self.module.app.test_client()

    def tearDown(self):
        if self.previous_base_dir is None:
            os.environ.pop("BASE_DIR", None)
        else:
            os.environ["BASE_DIR"] = self.previous_base_dir

        self.temp_dir.cleanup()
        if "progressbarServer" in sys.modules:
            del sys.modules["progressbarServer"]

    def _write(self, filename: str, content: str):
        path = Path(self.temp_dir.name) / filename
        path.write_text(content, encoding="utf-8")
        return path

    def _write_bytes(self, filename: str, content: bytes):
        path = Path(self.temp_dir.name) / filename
        path.write_bytes(content)
        return path

    def test_progress_reads_progress_file(self):
        self._write("progress.txt", "37.5")

        response = self.client.get("/progress")
        payload = response.get_json()

        self.assertEqual(response.status_code, 200)
        self.assertEqual(payload["progress"], 37.5)
        self.assertEqual(payload["source"], "progress.txt")

    def test_progress_falls_back_to_percent_file(self):
        self._write("progress.txt", "not-a-number")
        self._write("progressPercent.txt", "42%")

        response = self.client.get("/progress")
        payload = response.get_json()

        self.assertEqual(response.status_code, 200)
        self.assertEqual(payload["progress"], 42.0)
        self.assertEqual(payload["source"], "progressPercent.txt")

    def test_progress_returns_error_when_files_missing(self):
        response = self.client.get("/progress")
        payload = response.get_json()

        self.assertEqual(response.status_code, 200)
        self.assertIsNone(payload["progress"])
        self.assertIsNone(payload["source"])
        self.assertIn("No readable progress value found", payload["error"])

    def test_diagnostics_report_file_status(self):
        self._write("progress.txt", "51")

        response = self.client.get("/diagnostics/progress")
        payload = response.get_json()
        files_by_name = {entry["filename"]: entry for entry in payload["files"]}

        self.assertEqual(response.status_code, 200)
        self.assertEqual(payload["selected_progress"], 51.0)
        self.assertEqual(payload["selected_source"], "progress.txt")
        self.assertTrue(files_by_name["progress.txt"]["exists"])
        self.assertEqual(files_by_name["progress.txt"]["parsed_progress"], 51.0)
        self.assertFalse(files_by_name["progressPercent.txt"]["exists"])

    def test_progress_reflects_file_updates(self):
        self._write("progress.txt", "5")
        first = self.client.get("/progress").get_json()["progress"]

        self._write("progress.txt", "73")
        second = self.client.get("/progress").get_json()["progress"]

        self.assertEqual(first, 5.0)
        self.assertEqual(second, 73.0)

    def test_job_info_contains_preview_image_url(self):
        response = self.client.get("/job-info")
        payload = response.get_json()

        self.assertEqual(response.status_code, 200)
        self.assertIn("preview_image_url", payload)
        self.assertEqual(payload["preview_image_url"], "/print-preview-image")
        self.assertIn("runtime", payload)
        self.assertIn("telemetry", payload)

    def test_job_info_includes_runtime_and_telemetry(self):
        self._write("remaining_time.txt", "-1h20m")
        self._write("layerOverview.txt", "Layer: 10 / 80")
        self._write(
            "writerStatus.json",
            '{"state":"mqtt_message_received","updated_at":"2026-01-01T00:00:00Z"}'
        )

        response = self.client.get("/job-info")
        payload = response.get_json()

        self.assertEqual(payload["telemetry"]["remaining_time"], "-1h20m")
        self.assertEqual(payload["telemetry"]["layer"], "Layer: 10 / 80")
        self.assertEqual(payload["runtime"]["display_state"], "live")
        self.assertFalse(payload["runtime"]["stale"])

    def test_print_preview_image_returns_placeholder_when_missing(self):
        response = self.client.get("/print-preview-image")

        self.assertEqual(response.status_code, 200)
        self.assertIn("image/svg+xml", response.content_type)
        self.assertIn(b"<svg", response.data)

    def test_print_preview_image_serves_cover_file(self):
        self._write_bytes("printCover.png", b"mock-png-bytes")

        response = self.client.get("/print-preview-image")
        try:
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.data, b"mock-png-bytes")
        finally:
            response.close()

    def test_print_preview_image_uses_detected_image_mime(self):
        self._write_bytes("printCover.png", b"\xff\xd8\xffmock-jpeg")

        response = self.client.get("/print-preview-image")
        try:
            self.assertEqual(response.mimetype, "image/jpeg")
        finally:
            response.close()

    def test_print_preview_image_reflects_cover_updates(self):
        self._write_bytes("printCover.png", b"image-version-1")
        first = self.client.get("/print-preview-image")
        try:
            first_bytes = first.data
        finally:
            first.close()

        self._write_bytes("printCover.png", b"image-version-2")
        second = self.client.get("/print-preview-image")
        try:
            second_bytes = second.data
        finally:
            second.close()

        self.assertEqual(first_bytes, b"image-version-1")
        self.assertEqual(second_bytes, b"image-version-2")
        self.assertNotEqual(first_bytes, second_bytes)

    def test_print_preview_image_switches_from_placeholder_to_cover(self):
        first = self.client.get("/print-preview-image")
        try:
            first_content_type = first.content_type
            first_bytes = first.data
        finally:
            first.close()

        self._write_bytes("printCover.png", b"new-cover-bytes")

        second = self.client.get("/print-preview-image")
        try:
            second_content_type = second.content_type
            second_bytes = second.data
        finally:
            second.close()

        self.assertIn("image/svg+xml", first_content_type)
        self.assertIn(b"<svg", first_bytes)
        self.assertEqual(second_bytes, b"new-cover-bytes")
        self.assertNotIn("image/svg+xml", second_content_type)

    def test_printpreview_view_exists(self):
        response = self.client.get("/view/printpreview")

        self.assertEqual(response.status_code, 200)
        self.assertIn(b"/print-preview-image", response.data)
        self.assertIn(b"id=\"print-preview\"", response.data)

    def test_progressbar_view_is_self_contained_and_has_status_fields(self):
        response = self.client.get("/view/progressbar")

        self.assertEqual(response.status_code, 200)
        self.assertNotIn(b"stackpath.bootstrapcdn.com", response.data)
        self.assertIn(b"id=\"status-pill\"", response.data)
        self.assertIn(b"id=\"progress-value\"", response.data)

    def test_json_responses_disable_caching_without_global_cors(self):
        response = self.client.get("/job-info", headers={"Origin": "https://example.com"})

        self.assertIn("no-store", response.headers["Cache-Control"])
        self.assertNotIn("Access-Control-Allow-Origin", response.headers)

    def test_writer_diagnostics_missing_status_file(self):
        response = self.client.get("/diagnostics/writer")
        payload = response.get_json()

        self.assertEqual(response.status_code, 200)
        self.assertFalse(payload["status_file_exists"])
        self.assertIsNone(payload["writer_status"])
        self.assertIn("writerStatus.json not found", payload["error"])

    def test_writer_diagnostics_reads_status_file(self):
        status_payload = {
            "state": "mqtt_connected",
            "mqtt_message_count": 12,
            "last_write_file": "progress.txt"
        }
        self._write("writerStatus.json", '{"state":"mqtt_connected","mqtt_message_count":12,"last_write_file":"progress.txt"}')

        response = self.client.get("/diagnostics/writer")
        payload = response.get_json()

        self.assertEqual(response.status_code, 200)
        self.assertTrue(payload["status_file_exists"])
        self.assertEqual(payload["writer_status"], status_payload)
        self.assertIn("status_file_age_seconds", payload)

    def test_writer_diagnostics_hide_parse_error_details(self):
        self._write("writerStatus.json", "{not json")

        with self.assertLogs(self.module.app.logger, level="WARNING"):
            response = self.client.get("/diagnostics/writer")
        payload = response.get_json()

        self.assertEqual(response.status_code, 200)
        self.assertIsNone(payload["writer_status"])
        self.assertEqual(
            payload["error"],
            "writerStatus.json is not valid JSON. See the server log for details."
        )

    def test_progress_diagnostics_hide_read_error_details(self):
        (Path(self.temp_dir.name) / "progress.txt").mkdir()

        with self.assertLogs(self.module.app.logger, level="WARNING"):
            response = self.client.get("/diagnostics/progress")
        files_by_name = {entry["filename"]: entry for entry in response.get_json()["files"]}

        self.assertEqual(
            files_by_name["progress.txt"]["error"],
            "Could not read the file. See the server log for details."
        )

    def test_progress_error_hides_exception_details(self):
        failure = RuntimeError("secret detail")
        with patch.object(self.module, "read_progress_details", side_effect=failure), \
                self.assertLogs(self.module.app.logger, level="ERROR"):
            response = self.client.get("/progress")

        self.assertEqual(response.status_code, 500)
        self.assertNotIn(b"secret detail", response.data)
        self.assertEqual(
            response.get_json()["error"],
            "Failed to read progress. See the server log for details."
        )
        self.assertIn("no-store", response.headers["Cache-Control"])

    def test_svg_view_links_known_files(self):
        for name in self.module.SVG_FILES:
            with self.subTest(name):
                response = self.client.get(f"/view/{name}")

                self.assertEqual(response.status_code, 200)
                self.assertIn(f'src="/svg/{name}"'.encode(), response.data)
                self.assertIn(f'new EventSource("/updates/{name}")'.encode(), response.data)

    def test_svg_view_and_updates_reject_unknown_names(self):
        for name in ("progress.txt", "{{ 7 * 7 }}", "Filaments.svg<script>", "filaments.svg"):
            for route in ("/view/", "/updates/"):
                with self.subTest(route=route, name=name):
                    response = self.client.get(route + quote(name))

                    self.assertEqual(response.status_code, 404)
                    self.assertNotIn(b"49", response.data)
                    self.assertNotIn(b"<script>", response.data)

    def test_svg_updates_stream_known_files(self):
        self._write("Filaments.svg", "<svg/>")

        response = self.client.get("/updates/Filaments.svg")
        try:
            self.assertEqual(response.status_code, 200)
            self.assertEqual(response.mimetype, "text/event-stream")
        finally:
            response.close()


if __name__ == "__main__":
    unittest.main()
