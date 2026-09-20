[English](README.md) | [中文](README_zh.md)

# picopaste

One hotkey replaces the whole upload-and-paste dance: press `alt+shift+v` with a screenshot on the
clipboard, the image is uploaded to Linux, and its absolute remote path is written to the clipboard
and typed into the focused window. Claude Code can read the image straight from that path — nothing
else to do, the path is already at the cursor.

Codex CLI is the primary target, and Claude Code reads the same path; the remote only needs its
stock `sshd` and `sftp-server`.

## Getting started

Grab `picopaste.exe` from a release (or build it), and put `picopaste.ini` **next to it**:

```ini
[picopaste]
host = user@your-linux-host
hotkey = alt+shift+v
```

Then run it. The tray icon appears and connects immediately: yellow for a moment at most, green once
the channel is up. From then on, take a screenshot and press the hotkey.

## Install directory

Use an ASCII-only path such as `C:\tools\picopaste`; avoid non-English characters.
`picopaste.exe`, `picopaste.ini` and `README.md` must stay in the same directory.

## Prerequisites and troubleshooting

Windows must be able to log in to the Linux host without a password; verify by hand in cmd first:

```text
ssh user@your-linux-host
```

Run `picopaste.exe --selftest` to check the clipboard, image encoding, hotkey, memory and SFTP.

| Symptom | Action |
|---|---|
| `host` is reported empty | Keep the INI beside the exe and set `host` |
| Configuration is ignored | Move the directory to an ASCII-only path |
| Tray stays yellow or turns red | Verify passwordless ssh works by hand |
| Hotkey does nothing | Pick another `hotkey`, for example `ctrl+alt+p` |
| Large screenshots fail | Raise `job_memory_limit_mb` |

## Build and source

```bash
cmake -S . -B build -DPICOPASTE_BUILD_TESTS=ON \
  -DPICOPASTE_NEWOSP_DIR=$HOME/newosp-windows
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

- GitLab: http://172.16.40.222/dgliu/picopaste
- GitHub: https://github.com/DeguiLiu/picopaste
