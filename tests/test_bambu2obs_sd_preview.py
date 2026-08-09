import importlib
import io
import os
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, call, patch


REPO_ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = REPO_ROOT / "src"

if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))


class SdPreviewFallbackTests(unittest.TestCase):
    PNG_BYTES = b"\x89PNG\r\n\x1a\nmock-image-data"

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.previous_base_dir = os.environ.get("BASE_DIR")
        os.environ["BASE_DIR"] = self.temp_dir.name

        if "bambu2obs" in sys.modules:
            del sys.modules["bambu2obs"]

        self.module = importlib.import_module("bambu2obs")

    def tearDown(self):
        if self.previous_base_dir is None:
            os.environ.pop("BASE_DIR", None)
        else:
            os.environ["BASE_DIR"] = self.previous_base_dir

        self.temp_dir.cleanup()
        if "bambu2obs" in sys.modules:
            del sys.modules["bambu2obs"]

    def _write(self, filename: str, content: str):
        path = Path(self.temp_dir.name) / filename
        path.write_text(content, encoding="utf-8")
        return path

    def _image_response(self):
        response = MagicMock()
        response.ok = True
        response.status_code = 200
        response.headers = {"Content-Length": str(len(self.PNG_BYTES))}
        response.iter_content.return_value = [self.PNG_BYTES]
        return response

    def test_extract_preview_name_candidates_ignores_internal_plate_name(self):
        task_data = {"title": "Cube.gcode.3mf"}
        print_data = {"subtask_name": "Cube", "gcode_file": "/data/Metadata/plate_1.gcode"}

        candidates = self.module._extract_preview_name_candidates(task_data=task_data, print_data=print_data)

        self.assertIn("Cube", candidates)
        self.assertNotIn("plate_1", [candidate.lower() for candidate in candidates])

    def test_parse_ftp_list_line_with_spaces(self):
        line = "-rw-r--r--    1 1002     1002        39605 Dec 17 20:44 Cube Test.gcode.3mf"

        parsed = self.module._parse_ftp_list_line(line)

        self.assertIsNotNone(parsed)
        self.assertEqual(parsed["name"], "Cube Test.gcode.3mf")
        self.assertEqual(parsed["size"], 39605)
        self.assertFalse(parsed["is_dir"])
        self.assertIsNotNone(parsed["mtime"])

    def test_select_best_archive_prefers_name_match(self):
        entries = [
            {"name": "OtherProject.gcode.3mf", "mtime": None, "size": 1000},
            {"name": "Cube.gcode.3mf", "mtime": None, "size": 900},
        ]

        selected, mode = self.module._select_best_archive(entries, ["cube"])

        self.assertEqual(selected["name"], "Cube.gcode.3mf")
        self.assertEqual(mode, "name_match")

    def test_extract_preview_bytes_from_3mf_prefers_plate_image(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("Metadata/top_1.png", self.PNG_BYTES + b"-top")
            archive.writestr("Metadata/plate_1.png", self.PNG_BYTES + b"-plate")
            archive.writestr("Metadata/pick_1.png", self.PNG_BYTES + b"-pick")

        preview_bytes, preview_member = self.module._extract_preview_bytes_from_3mf(buffer.getvalue())

        self.assertEqual(preview_member, "Metadata/plate_1.png")
        self.assertEqual(preview_bytes, self.PNG_BYTES + b"-plate")

    def test_extract_preview_bytes_from_3mf_returns_none_when_missing(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("Metadata/readme.txt", b"no image")

        preview_bytes, preview_member = self.module._extract_preview_bytes_from_3mf(buffer.getvalue())

        self.assertIsNone(preview_bytes)
        self.assertIsNone(preview_member)

    def test_handle_print_data_attempts_sd_preview_even_with_existing_cover(self):
        Path(self.module.PRINT_COVER_IMAGE_PATH).write_bytes(b"old-cover")

        print_data = {
            "print_type": "local",
            "gcode_state": "RUNNING",
            "subtask_name": "Job Alpha",
            "gcode_file": "/data/Metadata/plate_2.gcode"
        }

        with patch.object(self.module, "request_sdcard_preview_fallback", return_value=False) as fallback_mock:
            self.module.handle_print_data(print_data)

        fallback_mock.assert_called_once()
        self.assertEqual(fallback_mock.call_args.kwargs["print_data"], print_data)
        self.assertFalse(fallback_mock.call_args.kwargs["force"])

    def test_handle_print_data_clears_stale_cover_on_job_change(self):
        Path(self.module.PRINT_COVER_IMAGE_PATH).write_bytes(b"old-cover")
        Path(self.module.PRINT_COVER_LEGACY_PATH).write_bytes(b"old-cover-legacy")

        with self.module.sd_preview_lock:
            self.module.sd_preview_attempt_state["job_key"] = "old_job_key"
            self.module.sd_preview_attempt_state["attempted_at"] = time.time()
            self.module.sd_preview_attempt_state["success"] = True

        print_data = {
            "print_type": "local",
            "gcode_state": "RUNNING",
            "subtask_name": "New Job",
            "gcode_file": "/data/Metadata/plate_4.gcode"
        }

        with patch.object(self.module, "request_sdcard_preview_fallback", return_value=False):
            self.module.handle_print_data(print_data)

        self.assertFalse(Path(self.module.PRINT_COVER_IMAGE_PATH).exists())
        self.assertFalse(Path(self.module.PRINT_COVER_LEGACY_PATH).exists())
        print_cover_text_path = Path(self.temp_dir.name) / "printCover.txt"
        self.assertTrue(print_cover_text_path.exists())
        self.assertEqual(print_cover_text_path.read_text(encoding="utf-8").strip(), "N/A")

    def test_extract_task_preview_urls_prefers_cover_then_thumbnail(self):
        task_data = {
            "cover": "https://example.com/cover.png",
            "thumbnail": "https://example.com/thumb.png"
        }

        urls = self.module.extract_task_preview_urls(task_data)

        self.assertEqual(
            urls,
            ["https://example.com/cover.png", "https://example.com/thumb.png"]
        )

    def test_extract_task_preview_urls_reads_nested_task_message_and_deduplicates(self):
        task_data = {
            "cover": "https://example.com/cover.png",
            "taskMessage": {
                "cover": "https://example.com/cover.png",
                "thumbnail": "https://example.com/thumb.png"
            }
        }

        urls = self.module.extract_task_preview_urls(task_data)

        self.assertEqual(
            urls,
            ["https://example.com/cover.png", "https://example.com/thumb.png"]
        )

    def test_successful_sd_preview_is_not_downloaded_again_after_retry_interval(self):
        with self.module.sd_preview_lock:
            self.module.sd_preview_attempt_state.update({
                "job_key": "same-job",
                "attempted_at": time.time() - self.module.SD_PREVIEW_RETRY_SECONDS - 1,
                "success": True
            })

        self.assertFalse(self.module._should_attempt_sd_preview("same-job", force=False))

    def test_failed_sd_preview_can_retry_after_interval(self):
        with self.module.sd_preview_lock:
            self.module.sd_preview_attempt_state.update({
                "job_key": "same-job",
                "attempted_at": time.time() - self.module.SD_PREVIEW_RETRY_SECONDS - 1,
                "success": False
            })

        self.assertTrue(self.module._should_attempt_sd_preview("same-job", force=False))

    def test_sd_preview_only_runs_for_active_local_prints(self):
        self.assertTrue(self.module.is_active_local_print({"print_type": "local", "gcode_state": "RUNNING"}))
        self.assertTrue(self.module.is_active_local_print({"print_type": "lan", "gcode_state": "PAUSE"}))
        self.assertFalse(self.module.is_active_local_print({"print_type": "cloud", "gcode_state": "RUNNING"}))
        self.assertFalse(self.module.is_active_local_print({"print_type": "local", "gcode_state": "IDLE"}))
        self.assertFalse(self.module.is_active_local_print({}))

    def test_numeric_print_stage_uses_pybambu_integer_mapping(self):
        with (
            patch.object(self.module, "write_to_file") as write_mock,
            patch.object(self.module, "request_sdcard_preview_fallback")
        ):
            self.module.handle_print_data({"mc_print_stage": "2"})

        self.assertIn(call("printStage", self.module.CURRENT_STAGE_IDS[2]), write_mock.call_args_list)

    def test_multiple_ams_units_receive_distinct_canonical_filenames(self):
        print_data = {
            "ams": {
                "ams": [
                    {"id": "0", "tray": [{"id": "0", "tray_info_idx": "GFB00", "tray_color": "FF0000"}]},
                    {"id": "1", "tray": [{"id": "0", "tray_info_idx": "GFB01", "tray_color": "00FF00"}]}
                ]
            }
        }

        with patch.object(self.module, "write_to_file") as write_mock:
            self.module.handle_print_data(print_data)

        written_names = [args.args[0] for args in write_mock.call_args_list]
        self.assertIn("ams1Tray1FilamentId", written_names)
        self.assertIn("ams2Tray1FilamentId", written_names)
        self.assertIn("ams1FilamentId", written_names)

    def test_svg_outputs_follow_custom_base_dir(self):
        self._write("ams1FilamentColor.txt", "FF0000")
        self._write("activeAmsTray.txt", "1")

        self.module.update_svg_with_all_tray_colors()

        self.assertTrue((Path(self.temp_dir.name) / "Filaments.svg").exists())
        self.assertTrue((Path(self.temp_dir.name) / "ActiveFilament.svg").exists())

    def test_preview_url_rejects_plain_http_and_private_ip(self):
        with self.assertRaises(ValueError):
            self.module.validate_remote_preview_url("http://example.com/cover.png")
        with self.assertRaises(ValueError):
            self.module.validate_remote_preview_url("https://127.0.0.1/cover.png")

    def test_save_print_cover_rejects_non_image_bytes(self):
        saved = self.module.save_print_cover_image(b"not-an-image", "test")

        self.assertFalse(saved)
        self.assertFalse(Path(self.module.PRINT_COVER_IMAGE_PATH).exists())

    def test_external_preview_download_never_receives_bearer_token(self):
        response = self._image_response()
        cloud = SimpleNamespace(
            session=MagicMock(),
            auth_token="top-secret-token",
            region="global",
            login=MagicMock()
        )

        with patch.object(self.module.requests, "get", return_value=response) as request_mock:
            image_bytes, mode = self.module.download_cloud_preview_image(
                cloud,
                "https://cdn.example.com/cover.png?signature=secret"
            )

        self.assertEqual(image_bytes, self.PNG_BYTES)
        self.assertEqual(mode, "plain")
        self.assertIsNone(request_mock.call_args.kwargs["headers"])
        cloud.session.get.assert_not_called()

    def test_bambu_api_preview_can_receive_bearer_token(self):
        response = self._image_response()
        session = MagicMock()
        session.get.return_value = response
        cloud = SimpleNamespace(
            session=session,
            auth_token="top-secret-token",
            region="global",
            login=MagicMock()
        )

        image_bytes, mode = self.module.download_cloud_preview_image(
            cloud,
            "https://api.bambulab.com/preview/cover.png"
        )

        self.assertEqual(image_bytes, self.PNG_BYTES)
        self.assertEqual(mode, "authorized")
        self.assertEqual(
            session.get.call_args.kwargs["headers"],
            {"Authorization": "Bearer top-secret-token"}
        )

    def test_archive_member_count_is_bounded(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("Metadata/plate_1.png", self.PNG_BYTES)
            archive.writestr("Metadata/extra.png", self.PNG_BYTES)

        with patch.object(self.module, "MAX_3MF_MEMBER_COUNT", 1):
            with self.assertRaisesRegex(ValueError, "more than 1 members"):
                self.module._extract_preview_bytes_from_3mf(buffer.getvalue())

    def test_diagnostic_redaction_removes_nested_secrets(self):
        redacted = self.module.redact_diagnostic_data({
            "token": "secret-token",
            "device": {"access_code": "12345678", "state": "RUNNING"}
        })

        self.assertEqual(redacted["token"], "<redacted>")
        self.assertEqual(redacted["device"]["access_code"], "<redacted>")
        self.assertEqual(redacted["device"]["state"], "RUNNING")


if __name__ == "__main__":
    unittest.main()
