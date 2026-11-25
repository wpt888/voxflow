"""Run the real paste expression against a synthetic Hyprland window."""

import os
from pathlib import Path
import subprocess
import sys

if not os.getenv("VX_TEST_PASTE"):
    sys.exit(1)

command, *arguments = sys.argv[1:]
paste = arguments and "send_key_state" in arguments[0]
if command == "locked":
    print(os.getenv("VX_SESSION_LOCKED", "false"))
    sys.exit(0)
if command != "repl" or len(arguments) != 1:
    sys.exit(1)
if paste:
    assert Path(os.environ["VX_CLIPBOARD"]).read_text() == " Gata. Café!\n"

prelude = """
local function window()
    return {
        stable_id = tonumber(os.getenv("VX_WINDOW_ID")),
        class = os.getenv("VX_WINDOW_CLASS"),
        accepts_input = true,
        tags = {}
    }
end

hl = {
    get_active_window = window,
    dsp = {
        send_key_state = function(options) return options end
    },
    dispatch = function(options)
        assert(options.key == "V")
        assert(options.window == nil)
        local trace = assert(io.open(os.getenv("VX_TRACE"), "a"))
        trace:write("paste ", options.mods, " ", options.state, "\\n")
        trace:close()
        return { ok = true }
    end
}
"""

env = os.environ | {
    "VX_WINDOW_ID": os.getenv("VX_PASTE_WINDOW_ID", "42") if paste else "42",
}
script = prelude + "\nprint((function()\n" + arguments[0] + "\nend)())\n"
result = subprocess.run(["lua", "-"], input=script, text=True, check=False, env=env)
sys.exit(result.returncode)
