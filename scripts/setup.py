"""Install VoxFlow and persist its Hyprland keybindings."""

import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARKER = "BEGIN VOXFLOW KEYBINDINGS"
SUPERWHISPER_BLOCK = re.compile(
    r"^-- BEGIN SUPERWHISPER KEYBINDINGS\r?\n(.*?)^-- END SUPERWHISPER KEYBINDINGS(?=\r?$)",
    re.MULTILINE | re.DOTALL,
)


def read_config(path):
    with path.open(encoding="utf-8", newline="") as file:
        return file.read()


def config_files():
    configured = os.environ.get("HYPRLAND_CONFIG")
    directory = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "hypr"
    if configured:
        main = Path(configured).expanduser().resolve()
    else:
        main = directory / "hyprland.lua"
        if not main.is_file():
            main = directory / "hyprland.conf"
        main = main.resolve()
    if not main.is_file() or main.suffix not in (".lua", ".conf"):
        raise ValueError("Could not find a Hyprland config. Set HYPRLAND_CONFIG to its path.")

    target = main
    omarchy_bindings = main.with_name("bindings.lua")
    if main.suffix == ".lua" and omarchy_bindings.is_file():
        if re.search(r"""^\s*require\s*\(\s*["']hypr\.bindings["']\s*\)""",
                     read_config(main), re.MULTILINE):
            target = omarchy_bindings.resolve()
    return main, target


def already_configured(path):
    data = read_config(path)
    if MARKER in data:
        return True
    comment = "--" if path.suffix == ".lua" else "#"
    return any(
        "voxflow" in line.lower() and not line.lstrip().startswith(comment)
        for line in data.splitlines()
    )


def binding_block(extension, executable):
    original = (ROOT / f"hyprland{extension}").read_text()
    body = original.split("\n", 1)[1].rstrip("\n")
    command = shlex.quote(str(executable))
    if extension == ".lua":
        comment = "--"
        body = body.replace('local voxflow = \'"$HOME/.local/bin/voxflow"\'',
                            f"local voxflow = {json.dumps(command, ensure_ascii=False)}")
    else:
        comment = "#"
        body = body.replace("$voxflow = ~/.local/bin/voxflow", f"$voxflow = {command}")
    return f"{comment} {MARKER}\n{body}\n{comment} END VOXFLOW KEYBINDINGS\n"


def backup_bindings(original, prefix):
    matches = list(SUPERWHISPER_BLOCK.finditer(original))
    if len(matches) != 1:
        raise ValueError("Backup setup needs one marked Superwhisper binding block in bindings.lua.")
    match = matches[0]
    body = match[1]
    program = re.search(r'^([ \t]*)local superwhisper = ("(?:[^"\\]|\\.)+")(?=\r?$)',
                        body, re.MULTILINE)
    if not program or any('superwhisper .. " ' + action + '"' not in body
                          for action in ("start", "stop", "cancel")):
        raise ValueError("Backup setup could not recognize the Superwhisper binding commands.")

    superwhisper = Path(json.loads(program[2]))
    if not superwhisper.is_absolute() or not os.access(superwhisper, os.X_OK):
        raise ValueError(f"Could not run Superwhisper at {superwhisper}.")

    command = shlex.join([str(prefix / "libexec/voxflow-dictation-router"),
                          str(prefix / "bin/voxflow"), str(superwhisper)])
    body = body[:program.start()] + f"{program[1]}local voxflow = " + json.dumps(
        command, ensure_ascii=False,
    ) + body[program.end():]
    for before, after in (("start", "press"), ("stop", "release"), ("cancel", "cancel")):
        body = body.replace(f'superwhisper .. " {before}"', f'voxflow .. " {after}"')
    body = body.replace("Superwhisper dictation", "dictation")
    newline = "\r\n" if "\r\n" in original else "\n"
    return (original[:match.start()] + f"-- {MARKER}{newline}" + body
            + "-- END VOXFLOW KEYBINDINGS" + original[match.end():])


def write_temporary(path, text, label, mode):
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", newline="", dir=path.parent,
        prefix=f"{path.name}.{label}-", delete=False,
    ) as file:
        temporary = Path(file.name)
        try:
            file.write(text)
            file.flush()
            os.fchmod(file.fileno(), mode)
        except BaseException:
            temporary.unlink()
            raise
    return temporary


def replace_config(path, expected, updated):
    mode = stat.S_IMODE(path.stat().st_mode)
    temporary = write_temporary(path, updated, "voxflow-tmp", mode)
    try:
        if read_config(path) != expected:
            raise ValueError(f"{path} changed during setup. Run make setup again.")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def hyprctl(*arguments):
    if not os.environ.get("HYPRLAND_INSTANCE_SIGNATURE"):
        return None
    try:
        result = subprocess.run(
            ["hyprctl", *arguments], capture_output=True, text=True, timeout=5, check=True,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip()


def config_errors():
    output = hyprctl("-j", "configerrors")
    if output is None:
        return None
    try:
        errors = json.loads(output)
    except json.JSONDecodeError:
        return None
    if isinstance(errors, list) and all(isinstance(error, str) for error in errors):
        return "\n".join(error for error in errors if error.strip())
    return None


def setup(prefix, destdir, backup_mode=False):
    if destdir:
        raise ValueError("For a staged install use make install with DESTDIR.")
    missing = [command for command in ("pw-record", "wl-copy", "stdbuf") if not shutil.which(command)]
    if missing:
        raise ValueError(f"Install the missing runtime commands first: {', '.join(missing)}.")

    main, target = config_files()
    original = read_config(target)
    existing = next((path for path in dict.fromkeys((target, main)) if already_configured(path)), None)
    errors_before = config_errors()

    backup = None
    updated = original
    if backup_mode:
        if "voxflow-dictation-router" in original:
            print(f"VoxFlow backup bindings are already configured in {target}.")
        elif target.suffix != ".lua" or existing:
            raise ValueError("Backup setup needs Superwhisper's Lua bindings without a separate VoxFlow binding.")
        else:
            updated = backup_bindings(original, prefix)
            subprocess.run([prefix / "bin/voxflow", "stop"], check=True, timeout=5)
    elif existing:
        print(f"VoxFlow keybindings are already configured in {existing}.")
    else:
        newline = "\r\n" if "\r\n" in original else "\n"
        block = binding_block(target.suffix, prefix / "bin/voxflow").replace("\n", newline)
        padding = newline if original.endswith(newline) else 2 * newline
        updated = original + (padding if original else "") + block

    if updated != original:
        backup = write_temporary(target, original, "voxflow-backup", 0o600)
        replace_config(target, original, updated)
        print(f"VoxFlow keybindings saved in {target}.")
        print(f"Previous config backed up to {backup}.")

    reloaded = hyprctl("reload")
    errors_after = config_errors()
    if reloaded != "ok" or errors_after is None:
        print("Live reload could not be verified. Run hyprctl reload from Hyprland or log in again.")
    elif errors_after and errors_after != errors_before:
        if backup and errors_before is not None:
            replace_config(target, updated, original)
            hyprctl("reload")
            config_errors()
            raise ValueError(f"Hyprland reported a new config error. The previous config was restored.\n{errors_after}")
        raise ValueError(f"Hyprland reported a config error.\n{errors_after}")
    elif errors_after:
        print("Hyprland still reports existing config errors. Check hyprctl configerrors.")
    elif backup_mode:
        print("Hyprland reloaded. The VoxFlow tray switch selects which app uses Right Alt.")
    else:
        print("Hyprland reloaded. Hold Right Alt to dictate and release it to transcribe.")


if __name__ == "__main__":
    try:
        prefix_arg, destdir_arg, *options = sys.argv[1:]
        if options not in ([], ["--superwhisper-backup"]):
            raise ValueError("Unknown setup option.")
        setup(Path(prefix_arg).expanduser().resolve(), destdir_arg, bool(options))
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"voxflow setup: {error}", file=sys.stderr)
        sys.exit(1)
