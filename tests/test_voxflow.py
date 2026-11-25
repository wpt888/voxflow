import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/voxflow-test"
RECORDER = ROOT / "build/test-recorder"

EXPECTED_TRANSCRIPT = " Gata. Café!\n"
RECORD_START = ["indicator ready", "recorder ready", "recording"]


class VoxFlowTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        directory = Path(self.tmp.name)
        self.trace = directory / "trace"
        self.clipboard = directory / "clipboard"
        self.auth = directory / "codex" / "auth.json"
        self.auth.parent.mkdir()
        tools = directory / "bin"
        tools.mkdir()
        (tools / "pw-record").symlink_to(RECORDER)
        self.executable(tools / "notify-send", "#!/bin/sh\nexit 0\n")
        self.executable(
            tools / "hyprctl",
            f"#!{sys.executable}\n" + (ROOT / "tests/hyprctl_fixture.py").read_text(),
        )
        self.executable(
            tools / "wl-copy",
            f"#!{sys.executable}\n"
            "import os, pathlib, sys, time\n"
            "if os.getenv('VX_CLIPBOARD_FAIL'): sys.exit(1)\n"
            "delay = os.getenv('VX_CLIPBOARD_DELAY')\n"
            "if delay:\n"
            "    with open(os.environ['VX_TRACE'], 'a') as trace: trace.write('copy started\\n')\n"
            "    time.sleep(float(delay))\n"
            "pathlib.Path(os.environ['VX_CLIPBOARD']).write_bytes(sys.stdin.buffer.read())\n",
        )
        self.env = dict(
            os.environ,
            XDG_RUNTIME_DIR=str(directory),
            WAYLAND_DISPLAY="voxflow-test",
            CODEX_HOME=str(self.auth.parent),
            PATH=str(tools) + os.pathsep + os.environ["PATH"],
            VX_TRACE=str(self.trace),
            VX_CLIPBOARD=str(self.clipboard),
            VX_EXPECT_TOKEN="fixture-token",
        )
        self.write_auth("fixture-token")

    @staticmethod
    def executable(path, script):
        path.write_text(script)
        path.chmod(0o700)

    def write_auth(self, token):
        self.auth.write_text(
            json.dumps({"tokens": {"access_token": token, "account_id": "fake-account"}})
        )
        self.auth.chmod(0o400)

    def events(self):
        return self.trace.read_text().splitlines() if self.trace.exists() else []

    def wait_for(self, event):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            if event in self.events():
                return
            time.sleep(0.005)
        self.fail(f"{event!r} did not happen. Saw {self.events()}.")

    def command(self, command):
        subprocess.run([BINARY, command], env=self.env, check=True, timeout=5)

    def status(self):
        return subprocess.check_output(
            [BINARY, "status"], env=self.env, text=True, timeout=2
        ).strip()

    def wait_for_status(self, expected):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            current = self.status()
            if current == expected:
                return
            time.sleep(0.01)
        self.fail(f"Expected status {expected!r}. Last status was {current!r}.")

    def start(self, **options):
        worker = subprocess.Popen(
            [BINARY, "press"], env=self.env | options,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self.addCleanup(self.reap, worker)
        return worker

    @staticmethod
    def reap(worker):
        if worker.poll() is None:
            worker.terminate()
        try:
            worker.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            worker.kill()
            worker.communicate()

    def finish(self, worker, error=None):
        output, stderr = worker.communicate(timeout=5)
        self.assertEqual(output, b"")
        self.assertEqual(worker.returncode, 1 if error else 0, stderr)
        self.assertNotIn(b"fixture-token", stderr)
        self.assertNotIn(EXPECTED_TRANSCRIPT.encode(), stderr)
        if error:
            self.assertIn(error, stderr.decode())
        else:
            self.assertEqual(stderr, b"")

    def release_after_audio(self):
        self.wait_for("recording")
        time.sleep(0.13)
        self.command("release")

    def test_success_and_fresh_read_only_credentials(self):
        self.env["VX_EXPECT_TOKEN"] = "fresh-fixture-token"
        worker = self.start(VX_RECORDER_DELAY_MS="180", VX_HTTP_DELAY_MS="200")
        self.wait_for("indicator ready")
        self.assertEqual(self.events(), ["indicator ready"])
        self.auth.unlink()
        self.write_auth("fresh-fixture-token")
        snapshot = (self.auth.read_bytes(), self.auth.stat().st_mtime_ns)
        self.release_after_audio()
        self.wait_for("upload")
        self.assertEqual(self.events(), RECORD_START + ["hidden", "upload"])
        self.assertFalse(self.clipboard.exists())
        self.finish(worker)
        self.assertEqual(self.events(), RECORD_START + ["hidden", "upload"])
        self.assertEqual(self.clipboard.read_text(), EXPECTED_TRANSCRIPT)
        self.assertEqual((self.auth.read_bytes(), self.auth.stat().st_mtime_ns), snapshot)

    def test_default_codex_home(self):
        self.auth.rename(self.auth.parent / "unused.json")
        self.env.pop("CODEX_HOME")
        self.env["HOME"] = self.env["XDG_RUNTIME_DIR"]
        self.auth = Path(self.env["HOME"]) / ".codex/auth.json"
        self.auth.parent.mkdir()
        self.write_auth("fixture-token")
        worker = self.start()
        self.release_after_audio()
        self.finish(worker)
        self.assertEqual(self.clipboard.read_text(), EXPECTED_TRANSCRIPT)

    def test_release_without_samples(self):
        worker = self.start(VX_RECORDER_DELAY_MS="500")
        self.wait_for("indicator ready")
        self.command("release")
        self.finish(worker)
        self.assertEqual(self.events(), ["indicator ready", "closed without mapping"])
        self.assertFalse(self.clipboard.exists())

    def test_release_handler_started_first(self):
        with subprocess.Popen([BINARY, "release"], env=self.env) as release:
            time.sleep(0.04)
            worker = self.start(VX_RECORDER_DELAY_MS="500")
            self.finish(worker)
            self.assertEqual(release.wait(timeout=2), 0)
        self.assertNotIn("recording", self.events())
        self.assertNotIn("upload", self.events())

    def test_cancel_before_release(self):
        worker = self.start()
        self.wait_for("recording")
        self.command("press")  # Ignore a second press while a capture owns the session.
        self.command("cancel")
        self.finish(worker)
        self.assertEqual(self.events(), RECORD_START + ["hidden"])
        self.assertFalse(self.clipboard.exists())

    def test_cancel_during_upload(self):
        worker = self.start(VX_HTTP_DELAY_MS="2000")
        self.release_after_audio()
        self.wait_for("upload")
        self.command("cancel")
        self.finish(worker)
        self.assertEqual(self.events(), RECORD_START + ["hidden", "upload"])
        self.assertFalse(self.clipboard.exists())

    def test_bar_status_follows_a_take(self):
        path = Path(subprocess.check_output(
            [BINARY, "status-path"], env=self.env, text=True, timeout=2
        ).strip())
        self.assertEqual(path.parent.parent, Path(self.env["XDG_RUNTIME_DIR"]))
        self.assertEqual(path.name, "lock")
        inode = path.stat().st_ino
        self.assertEqual(self.status(), "ready")

        worker = self.start(VX_RECORDER_DELAY_MS="200", VX_HTTP_DELAY_MS="400",
                            VX_CLIPBOARD_DELAY="0.3")
        self.wait_for("indicator ready")
        self.assertEqual(self.status(), "ready")
        self.wait_for_status("recording")
        self.release_after_audio()
        self.wait_for("upload")
        self.assertEqual(self.status(), "transcribing")
        self.wait_for("copy started")
        self.assertEqual(self.status(), "transcribing")
        self.finish(worker)
        self.assertEqual(self.status(), "ready")
        self.assertEqual(path.stat().st_ino, inode)

    def test_bar_status_clears_after_cancel_or_cloud_failure(self):
        worker = self.start()
        self.wait_for_status("recording")
        self.command("cancel")
        self.finish(worker)
        self.assertEqual(self.status(), "ready")
        self.trace.unlink()

        worker = self.start(VX_HTTP_STATUS="500", VX_HTTP_DELAY_MS="300")
        self.release_after_audio()
        self.wait_for("upload")
        self.assertEqual(self.status(), "transcribing")
        self.finish(worker, "service failed")
        self.assertEqual(self.status(), "ready")

    def test_bar_status_recovers_when_a_worker_is_killed(self):
        worker = self.start()
        self.wait_for_status("recording")
        worker.kill()
        worker.communicate(timeout=5)
        self.assertEqual(self.status(), "ready")

    def test_pause_stops_capture_and_resume_allows_a_new_take(self):
        worker = self.start()
        self.wait_for_status("recording")
        self.command("pause")
        self.finish(worker)
        self.assertEqual(self.status(), "paused")
        self.assertEqual(self.events(), RECORD_START + ["hidden"])
        self.assertFalse(self.clipboard.exists())

        self.trace.unlink()
        self.command("press")
        self.assertEqual(self.events(), [])
        self.assertEqual(self.status(), "paused")

        self.command("resume")
        self.assertEqual(self.status(), "ready")
        worker = self.start()
        self.release_after_audio()
        self.finish(worker)
        self.assertEqual(self.clipboard.read_text(), EXPECTED_TRANSCRIPT)

    def test_pause_during_transcription_discards_the_result(self):
        worker = self.start(VX_HTTP_DELAY_MS="2000")
        self.release_after_audio()
        self.wait_for("upload")
        self.command("pause")
        self.finish(worker)
        self.assertEqual(self.status(), "paused")
        self.assertFalse(self.clipboard.exists())

    def test_signal_during_recording(self):
        worker = self.start()
        self.wait_for("recording")
        worker.terminate()
        self.finish(worker)
        self.assertEqual(self.events(), RECORD_START + ["hidden"])

    def test_recorder_error_stays_hidden(self):
        worker = self.start(VX_RECORDER_FAIL="1")
        self.finish(worker, "pw-record exited")
        self.assertEqual(self.events(), ["indicator ready", "closed without mapping"])
        self.assertFalse(self.clipboard.exists())

    def test_pipewire_169_exit_status_after_release_still_transcribes(self):
        worker = self.start(VX_RECORDER_STOP_STATUS="1")
        self.release_after_audio()
        self.finish(worker)
        self.assertEqual(self.events(), RECORD_START + ["hidden", "upload"])
        self.assertEqual(self.clipboard.read_text(), EXPECTED_TRANSCRIPT)

    def test_unexpected_exit_during_capture_discards_audio(self):
        worker = self.start(VX_RECORDER_EXIT_AFTER_CHUNKS="10")
        self.finish(worker, "pw-record exited during capture")
        self.assertEqual(self.events(), RECORD_START + ["hidden"])
        self.assertFalse(self.clipboard.exists())

    def test_other_exit_status_after_release_still_reports_failure(self):
        worker = self.start(VX_RECORDER_STOP_STATUS="2")
        self.release_after_audio()
        self.finish(worker, "pw-record failed before it finished writing audio")
        self.assertEqual(self.events(), RECORD_START + ["hidden"])
        self.assertFalse(self.clipboard.exists())

    def test_waveform_reacts_to_audio_samples(self):
        worker = self.start(VX_TEST_WAVEFORM="1")
        self.wait_for("active waveform")
        self.command("cancel")
        self.finish(worker)
        self.assertEqual(self.events(), RECORD_START + ["silent waveform", "active waveform", "hidden"])

    def test_missing_or_invalid_credentials(self):
        for contents in (None, '{"OPENAI_API_KEY":"ignored"}', '{"tokens":{"access_token":"bad\\nvalue"}}'):
            with self.subTest(contents=contents):
                self.auth.unlink(missing_ok=True)
                if contents is not None:
                    self.auth.write_text(contents)
                self.trace.unlink(missing_ok=True)
                worker = self.start()
                self.release_after_audio()
                self.finish(worker, "Run codex login")
                self.assertEqual(self.events(), RECORD_START + ["hidden"])
                self.assertFalse(self.clipboard.exists())

    def test_cloud_failures_leave_existing_clipboard_intact(self):
        cases = [
            ({"VX_HTTP_STATUS": "401"}, "Run codex login"),
            ({"VX_HTTP_STATUS": "403"}, "HTTP 403"),
            ({"VX_HTTP_STATUS": "429"}, "rate-limited"),
            ({"VX_HTTP_STATUS": "500"}, "service failed"),
            ({"VX_HTTP_STATUS": "302"}, "rejected the transcription request"),
            ({"VX_HTTP_STATUS": "0", "VX_HTTP_ERROR": "The request timed out."}, "timed out"),
            ({"VX_HTTP_BODY": '{"text":"truncated'}, "invalid transcription response"),
            ({"VX_HTTP_BODY": '{"text":"valid"}{}'}, "invalid transcription response"),
            ({"VX_HTTP_BODY": '{"text":" "}'}, "empty transcript"),
        ]
        self.clipboard.write_text("original clipboard")
        for options, error in cases:
            with self.subTest(options=options):
                self.trace.unlink(missing_ok=True)
                worker = self.start(**options)
                self.release_after_audio()
                self.finish(worker, error)
                self.assertEqual(self.events(), RECORD_START + ["hidden", "upload"])
                self.assertEqual(self.clipboard.read_text(), "original clipboard")

    def test_clipboard_failure_hides_indicator(self):
        worker = self.start(VX_CLIPBOARD_FAIL="1")
        self.release_after_audio()
        self.finish(worker, "Could not copy")
        self.assertEqual(self.events(), RECORD_START + ["hidden", "upload"])

    def test_waveform_disappears_on_release_before_transcription_and_copy(self):
        worker = self.start(VX_RECORDER_STOP_DELAY_MS="300", VX_CLIPBOARD_DELAY="0.3")
        self.release_after_audio()
        self.wait_for("hidden")
        self.assertEqual(self.events(), RECORD_START + ["hidden"])
        self.assertIsNone(worker.poll())
        self.wait_for("copy started")
        self.assertEqual(self.events(), RECORD_START + ["hidden", "upload", "copy started"])
        self.assertFalse(self.clipboard.exists())
        self.finish(worker)
        self.assertEqual(self.events().count("hidden"), 1)
        self.assertEqual(self.clipboard.read_text(), EXPECTED_TRANSCRIPT)

    @unittest.skipUnless(shutil.which("lua"), "Lua is needed to run the Hyprland expression")
    def test_automatic_paste_uses_the_focused_editors_shortcut(self):
        worker = self.start(VX_TEST_PASTE="1", VX_WINDOW_CLASS="editor")
        self.release_after_audio()
        self.finish(worker)
        self.assertEqual(
            self.events(), RECORD_START + ["hidden", "upload", "paste CTRL down", "paste CTRL up"]
        )
        self.assertEqual(self.clipboard.read_text(), EXPECTED_TRANSCRIPT)

    @unittest.skipUnless(shutil.which("lua"), "Lua is needed to run the Hyprland expression")
    def test_automatic_paste_uses_the_terminal_shortcut(self):
        worker = self.start(VX_TEST_PASTE="1", VX_WINDOW_CLASS="org.wezfurlong.wezterm")
        self.release_after_audio()
        self.finish(worker)
        self.assertEqual(
            self.events(),
            RECORD_START + ["hidden", "upload", "paste CTRL SHIFT down", "paste CTRL SHIFT up"],
        )

    @unittest.skipUnless(shutil.which("lua"), "Lua is needed to run the Hyprland expression")
    def test_focus_change_or_session_lock_keeps_manual_paste_available(self):
        for options in ({"VX_PASTE_WINDOW_ID": "91"}, {"VX_SESSION_LOCKED": "true"}):
            with self.subTest(options=options):
                self.trace.unlink(missing_ok=True)
                self.clipboard.unlink(missing_ok=True)
                worker = self.start(VX_TEST_PASTE="1", VX_WINDOW_CLASS="editor", **options)
                self.release_after_audio()
                self.finish(worker)
                self.assertEqual(self.events(), RECORD_START + ["hidden", "upload"])
                self.assertEqual(self.clipboard.read_text(), EXPECTED_TRANSCRIPT)

def rss_kib(pid):
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        if line.startswith("Rss:"):
            return int(line.split()[1])
    raise RuntimeError("Rss field unavailable")


def measure():
    sample = VoxFlowTest()
    sample.setUp()
    try:
        worker = sample.start(VX_HTTP_DELAY_MS="2000")
        sample.wait_for("recording")
        time.sleep(3)
        children = Path(f"/proc/{worker.pid}/task/{worker.pid}/children").read_text()
        recorder, = map(int, children.split())
        print(f"Recording RSS after 3 s   VoxFlow {rss_kib(worker.pid)} KiB, "
              f"mock recorder {rss_kib(recorder)} KiB")
        sample.command("release")
        sample.wait_for("upload")
        print(f"Processing RSS            VoxFlow {rss_kib(worker.pid)} KiB")
        sample.finish(worker)
        assert not Path(f"/proc/{worker.pid}").exists()
        assert not Path(f"/proc/{recorder}").exists()
        print("Idle RSS                  0 KiB for VoxFlow and mock recorder")
    finally:
        sample.doCleanups()


if __name__ == "__main__":
    if sys.argv[1:] == ["--measure"]:
        measure()
    else:
        unittest.main(verbosity=2)
