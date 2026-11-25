"""Install the VoxFlow widget into Omarchy's user plugin directory."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PLUGIN_ID = "local.voxflow"
EXECUTABLE_EXPRESSION = 'Quickshell.env("HOME") + "/.local/bin/voxflow"'


def install_bar(prefix, plugin_root):
    if not shutil.which("omarchy"):
        raise ValueError("The VoxFlow bar widget requires Omarchy.")

    plugin_root.mkdir(parents=True, exist_ok=True)
    destination = plugin_root / PLUGIN_ID
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        raise ValueError(f"Expected a local plugin directory at {destination}.")

    with tempfile.TemporaryDirectory(prefix=".voxflow-", dir=plugin_root) as temp:
        staged = Path(temp) / PLUGIN_ID
        shutil.copytree(ROOT / "omarchy" / "voxflow", staged)
        widget = staged / "BarWidget.qml"
        text = widget.read_text(encoding="utf-8")
        if text.count(EXECUTABLE_EXPRESSION) != 1:
            raise ValueError("The VoxFlow bar widget is missing its executable setting.")
        executable = json.dumps(str(prefix / "bin" / "voxflow"), ensure_ascii=False)
        widget.write_text(text.replace(EXECUTABLE_EXPRESSION, executable), encoding="utf-8")
        subprocess.run(["omarchy", "plugin", "validate", str(staged)], check=True, timeout=10)

        if not destination.exists():
            staged.replace(destination)
        else:
            for source in staged.rglob("*"):
                if source.is_file():
                    target = destination / source.relative_to(staged)
                    if target.parent.is_symlink():
                        raise ValueError(f"Expected a local plugin directory at {target.parent}.")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source.replace(target)

    # Omarchy's put waits for discovery and leaves a previously placed widget in place.
    subprocess.run(["omarchy-shell", "shell", "rescanPlugins"], check=True, timeout=10)
    subprocess.run(["omarchy", "bar", "put", PLUGIN_ID, "--section", "right",
                    "--after", "omarchy.tray"], check=True, timeout=15)


if __name__ == "__main__":
    try:
        prefix_arg, destdir = sys.argv[1:]
        if destdir:
            raise ValueError("Run bar-setup inside your Omarchy session. Use make install for staging.")
        install_bar(Path(prefix_arg).expanduser().resolve(),
                    Path.home() / ".config" / "omarchy" / "plugins")
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"voxflow bar setup: {error}", file=sys.stderr)
        sys.exit(1)
