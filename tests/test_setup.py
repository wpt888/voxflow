import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
MARKER = "BEGIN VOXFLOW KEYBINDINGS"


class SetupTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.hypr = self.directory / "config/hypr"
        self.hypr.mkdir(parents=True)
        self.prefix = self.directory / "install prefix"
        self.tools = self.directory / "bin"
        self.tools.mkdir()
        for name in ("pw-record", "wl-copy", "stdbuf"):
            self.stub(name, "exit 0")
        self.env = os.environ | {
            "XDG_CONFIG_HOME": str(self.hypr.parent),
            "PATH": str(self.tools) + os.pathsep + os.environ["PATH"],
        }
        self.env.pop("HYPRLAND_CONFIG", None)
        self.env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
        self.env.pop("MAKEFLAGS", None)
        self.env.pop("MFLAGS", None)

    def stub(self, name, script):
        command = self.tools / name
        command.write_text("#!/bin/sh\n" + script + "\n")
        command.chmod(0o700)

    def setup_voxflow(self, *, success=True):
        result = subprocess.run(
            ["make", "setup", f"PREFIX={self.prefix}"], cwd=ROOT, env=self.env,
            capture_output=True, text=True, timeout=15,
        )
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_omarchy_persistent_binding_and_repeat_install(self):
        main = self.hypr / "hyprland.lua"
        main.write_text('require("hypr.bindings")\n')
        bindings = self.hypr / "bindings.lua"
        original = '-- existing shortcut\nhl.bind("SUPER + X", function() end)\n'
        bindings.write_text(original)
        bindings.chmod(0o640)

        self.setup_voxflow()
        updated = bindings.read_text()
        self.assertTrue(updated.startswith(original))
        self.assertIn(str(self.prefix / "bin/voxflow"), updated)
        self.assertIn('hl.unbind("ALT + Alt_R")', updated)
        self.assertIn('"SUPER + SHIFT + Escape"', updated)
        self.assertEqual(bindings.stat().st_mode & 0o777, 0o640)
        self.assertEqual(main.read_text(), 'require("hypr.bindings")\n')
        backup, = list(self.hypr.glob("bindings.lua.voxflow-backup-*"))
        self.assertEqual(backup.read_text(), original)
        self.assertTrue(os.access(self.prefix / "bin/voxflow", os.X_OK))
        icon = self.prefix / "share/icons/hicolor/256x256/apps/voxflow.png"
        self.assertEqual(icon.read_bytes(), (ROOT / "assets/voxflow.png").read_bytes())

        self.setup_voxflow()
        self.assertEqual(bindings.read_text(), updated)
        self.assertEqual(updated.count(MARKER), 1)
        self.assertEqual(list(self.hypr.glob("bindings.lua.voxflow-backup-*")), [backup])

    def test_legacy_config_keeps_its_line_endings(self):
        main = self.hypr / "hyprland.conf"
        original = b"# user overrides\r\n"
        main.write_bytes(original)
        self.setup_voxflow()
        updated = main.read_bytes()
        self.assertTrue(updated.startswith(original))
        self.assertIn(b"$voxflowKey = code:108\r\n", updated)
        self.assertIn(b"$voxflow release\r\n", updated)
        self.assertNotIn(b"\n", updated.replace(b"\r\n", b""))

    def test_existing_manual_binding_is_kept(self):
        main = self.hypr / "hyprland.lua"
        original = 'hl.bind("F9", hl.dsp.exec_cmd("voxflow press"))\n'
        main.write_text(original)
        self.setup_voxflow()
        self.assertEqual(main.read_text(), original)
        self.assertFalse(list(self.hypr.glob("*.voxflow-backup-*")))

    def test_reload_runs_and_a_new_config_error_rolls_back(self):
        main = self.hypr / "hyprland.lua"
        original = "-- original"
        main.write_text(original)
        log = self.directory / "hyprctl.log"
        self.env.update(
            HYPRLAND_INSTANCE_SIGNATURE="test-session",
            VOXFLOW_CONFIG_FIXTURE=str(main),
            VOXFLOW_HYPRCTL_LOG=str(log),
        )
        self.stub(
            "hyprctl",
            'echo "$*" >> "$VOXFLOW_HYPRCTL_LOG"\n'
            'if [ "$1" = reload ]; then echo ok; exit 0; fi\n'
            'if [ "$1 $2" != "-j configerrors" ]; then exit 1; fi\n'
            'if grep -q "BEGIN VOXFLOW KEYBINDINGS" "$VOXFLOW_CONFIG_FIXTURE"; then\n'
            """  echo '["fixture config error"]'\nelse\n  echo '[]'\nfi""",
        )
        result = self.setup_voxflow(success=False)
        self.assertIn("previous config was restored", result.stderr)
        self.assertEqual(main.read_text(), original)
        self.assertEqual(
            log.read_text().splitlines(),
            ["-j configerrors", "reload", "-j configerrors", "reload", "-j configerrors"],
        )

    def test_reload_success(self):
        (self.hypr / "hyprland.lua").write_text("-- original\n")
        self.env["HYPRLAND_INSTANCE_SIGNATURE"] = "test-session"
        self.stub(
            "hyprctl",
            'case "$*" in\n'
            '  reload) echo ok ;;\n'
            '  "-j configerrors") echo "[]" ;;\n'
            '  *) exit 1 ;;\nesac',
        )
        result = self.setup_voxflow()
        self.assertIn("Hyprland reloaded. Hold Right Alt", result.stdout)

    def test_omarchy_bar_widget_install_and_upgrade(self):
        install_bar = runpy.run_path(str(ROOT / "scripts/setup_bar.py"))["install_bar"]
        plugins = self.directory / "plugins"
        destination = plugins / "local.voxflow"
        with mock.patch("shutil.which", return_value="/usr/bin/omarchy"), \
             mock.patch("subprocess.run") as run:
            install_bar(self.prefix, plugins)
            self.assertIn(str(self.prefix / "bin/voxflow"),
                          (destination / "BarWidget.qml").read_text())
            self.assertEqual((destination / "voxflow.png").read_bytes(),
                             (ROOT / "assets/voxflow.png").read_bytes())
            self.assertTrue((destination / "i18n/en.json").is_file())
            marker = destination / "user-file"
            marker.write_text("keep")

            install_bar(self.prefix, plugins)
            self.assertEqual(marker.read_text(), "keep")
            self.assertEqual(run.call_count, 6)
            self.assertEqual(run.call_args.args[0],
                             ["omarchy", "bar", "put", "local.voxflow", "--section", "right",
                              "--after", "omarchy.tray"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
