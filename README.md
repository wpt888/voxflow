# VoxFlow

<img src="assets/voxflow.png" alt="VoxFlow icon" width="112" align="right">

Hold **Right Alt** to dictate in Hyprland. A tall, live waveform appears at
the bottom center when microphone samples start arriving. Release the key to
send the recording to ChatGPT. The waveform changes to an amber clock while
VoxFlow transcribes it. VoxFlow copies the result to the Wayland clipboard
and automatically pastes into the window that was focused on release if it
still has focus.

VoxFlow runs on demand. It uses `pw-record`, `wl-copy`, and a small,
click-through Wayland layer surface.

## Build and install

From the project directory, run this once as your normal user inside Hyprland.

```sh
make setup
```

Setup builds and installs VoxFlow in `~/.local`, adds the Right Alt bindings
to your Hyprland config, backs up the file it edits, and reloads Hyprland.
The bindings are available on future logins and launch VoxFlow on demand.

Build with a C compiler, Make, pkg-config, `wayland-scanner`, the Wayland
protocol XML files, and development headers for Wayland, Cairo, libcurl,
and json-c. On Arch and Omarchy, the corresponding packages are
`gcc make pkgconf wayland wayland-protocols cairo curl json-c`.
Setup and the test suite also use Python 3.10 or newer.

At runtime you need PipeWire with a working default capture source,
`pw-record`, `wl-copy`, and `stdbuf` from coreutils. If `notify-send` and a
notification service are available, they display errors from keybinding
runs. Terminal runs also print the error.

Use `make install` to install just the binary and app icon, or `make test` to
run the automated tests.

## Codex sign-in

Use a Codex CLI session signed into ChatGPT with file-based credentials.
VoxFlow reads `$CODEX_HOME/auth.json` once per upload, with
`~/.codex/auth.json` as the default. If Codex is configured for keyring or
ephemeral storage, set `cli_auth_credentials_store = "file"` in Codex's
`config.toml` and run `codex login` with your ChatGPT account. Run
`codex login` again if VoxFlow reports an expired or rejected session.
For a custom `CODEX_HOME`, make it available to both the CLI and the
Hyprland session. [Codex authentication guide](https://learn.chatgpt.com/docs/auth)

VoxFlow opens the credential file read-only and sends the access token
directly to `https://chatgpt.com/backend-api/transcribe`. It holds up to
75 seconds of 16 kHz mono PCM in memory, with a maximum audio payload of
approximately 2.4 MB. A take that hits the cap is discarded so the backend
cannot silently drop its tail. A valid take results in one multipart WAV
request. This is ChatGPT's internal dictation endpoint and its availability
can change.

## Configure Right Alt

For a manual configuration, add the block from [hyprland.lua](hyprland.lua) to your
`~/.config/hypr/hyprland.lua` on Hyprland 0.55 or newer. If you use a
hyprlang config, add the block from [hyprland.conf](hyprland.conf) to
`~/.config/hypr/hyprland.conf`. Run `hyprctl reload` and check
`hyprctl configerrors`.

The bindings reserve physical Right Alt, XKB keycode 108, including on
layouts that use it as AltGr. To use another key, change `dictation_key`
or `$voxflowKey` in your Hyprland config. On Omarchy, VoxFlow takes over
the optional built-in Right Alt dictation shortcut. **Super+Shift+Escape**
cancels the current take or request.
Only one take runs at a time.

## Test on your desktop

1. Focus an editor and hold **Right Alt** until the waveform appears at the bottom center.
2. Say “This is a VoxFlow test” and watch the waveform respond, then release **Right Alt**.
3. Leave the cursor in the editor. The transcript should appear when the
   amber clock disappears.
4. Hold the key again, then press **Super+Shift+Escape**. The badge should vanish and
   the previous clipboard value should still paste.

To test before adding any bindings, run these commands in a terminal inside
Hyprland from the built project. Speak after the waveform appears,
then run the release command.

```sh
./build/voxflow press &
# Speak, then run the next line in the same terminal.
./build/voxflow release
wait
wl-paste --no-newline
```

If focus changed or the compositor lacks automatic paste support, the result
is available on the clipboard. Use **Ctrl+V** in an editor or
**Ctrl+Shift+V** in most terminals. To see the latest capture, transcription,
clipboard, and automatic paste outcomes, read the journal.

```sh
journalctl -b -t voxflow -n 20 --no-pager
```

## Automated tests and memory

`make test` exercises the real controller and credential/response parsing
with a synthetic recorder, indicator event trace, mock HTTP transport, and
mock clipboard. It covers startup, fast release, cancellation, stale
credentials, rate limits, server and network failures, and incomplete
responses.

Sample RSS from Linux `smaps_rollup` on Omarchy x86-64, using the mock
components above:

| State | VoxFlow RSS | Recorder RSS |
| --- | ---: | ---: |
| Idle after the take | 0 KiB | 0 KiB |
| Recording at 3 seconds | 7,788 KiB | 1,828 KiB |
| Processing | 7,944 KiB | 0 KiB |

Reproduce the sample with `python3 tests/test_voxflow.py --measure` after
`make test`. The real `wl-copy` can stay alive while it owns the clipboard.
The build sandbox blocked access to Hyprland, PipeWire, the Wayland
clipboard, and ChatGPT, so the measurements use mocks and the live desktop
and cloud smoke tests remain to be run.

[MIT License](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md)
