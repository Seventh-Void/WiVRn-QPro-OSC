# fpsVR-style stats on the WayVR watch

A frame timing panel for [WayVR](https://github.com/wlx-team/wayvr)'s wrist watch, like fpsVR on SteamVR: per-frame CPU and GPU frame time graphs for the VR game you're in, FPS, temperatures, load and memory, plus how long you've been in the game (and, in VRChat, in the current world) under the clock.

<p align="center">
  <img src="watch.png" alt="WayVR watch with the stats panel" width="380">
  <img src="watch-hidden.png" alt="The same watch with the stats hidden" width="380">
</p>
<p align="center"><img src="fpsvr-graphs.gif" alt="Frame time graphs going from good to bad and back while VRChat loads a world" width="380"></p>

*All three are from a real VRChat session on a Quest Pro with this fork, rendered with WayVR's `uidev` previewer (the dark background stands in for the game behind the watch; battery levels are left empty by the previewer). Left: the stats panel. Right: the same watch with the stats hidden. The GIF replays 24 s of recorded VRChat frames, world loading included, through the panel's own graph and colour code: FPS and frame times turn orange and red while the frames are late, and back to white once it recovers. The temperature, load and memory lines in the GIF are from the moment of the screenshot.*

## What it shows

| Item | Source |
|---|---|
| Game name | WiVRn: the focused (non-overlay) OpenXR app. |
| Game time (under the clock) | Time since the panel first saw the game in focus (restarting the panel or WayVR restarts it). Changes to `No VR game` 5 s after the game stops delivering frames. |
| World time (under the clock) | VRChat only, with [VRCX](https://github.com/vrcx-team/VRCX) installed (see [VRCX](#vrcx-optional)): the current world and how long you've been in it. Blank otherwise. |
| FPS | Frames the game delivered in the last second. Shows `--` until a full second has passed. |
| Avg | Frames delivered since the panel first saw the game in focus, divided by the time since then. |
| GPU / CPU frame time and graphs | WiVRn, every frame of the focused game. CPU = app wake-up to `xrEndFrame`, GPU = `xrEndFrame` to its GPU work finishing. Graphs show the last 150 frames as bars; the dashed line is the frame budget for the headset's refresh rate. Frame time labels are the average over the last second. |
| GPU temperature and load | `amdgpu`: junction (hotspot) temperature and `gpu_busy_percent`. |
| VRAM | `amdgpu`: `mem_info_vram_used` / `mem_info_vram_total`. |
| CPU temperature and load | `k10temp` (Tctl) or `coretemp` (package) temperature; load from `/proc/stat`. |
| RAM | `/proc/meminfo`: total minus available. |

Nothing is estimated. Anything this machine can't read stays `--` (GPU stats need an AMD GPU; the CPU temperature is left out without `k10temp` or `coretemp`). The overlay itself (WayVR) is never counted: WiVRn only exports the focused game.

### Colours

The target is the headset's refresh rate (90 FPS at 90 Hz; frame budget 11.1 ms).

| | Fine | Orange | Red |
|---|---|---|---|
| Graph bars | green: within the budget | up to 25 % over | more than 25 % over |
| GPU / CPU ms | normal text colour | up to 25 % over | more than 25 % over |
| FPS / Avg | normal text colour | below 90 % of the target | below half the target |

### Hide and show

**Hide stats** (top left of the panel) hides the whole panel and leaves a **Show stats** button in its place. The watch keeps its size, so nothing else on it moves. While hidden, `wayvr-stats` stops reading frame times and sensors and sends nothing; only the game and world time under the clock keep counting. A new WayVR always starts with the stats shown.

### VRCX (optional)

The world time line comes from [VRCX](https://github.com/vrcx-team/VRCX), which logs every VRChat world you join. `wayvr-stats` reads VRCX's database (`~/.config/VRCX/VRCX.sqlite3`) read-only, every 5 s while VRChat is the focused game, and shows the newest world and how long ago you joined it (the same "time in current world" VRCX shows).

There is nothing to set up: if that database doesn't exist, the world time is off and the line stays blank (no errors, no work). Once VRCX is installed, it turns on by itself.

VRCX has to be **running** for the world time to show: only then does it log world changes, so while it's closed the line stays blank instead of showing a world you may have left. If you start VRCX after joining a world, the time shows from the next world you join.

## Requirements

- `wivrn-server` from this fork, **v26.9-qpro.3 or later** (it writes `/dev/shm/wivrn-frametime`). Stock WiVRn doesn't export frame times; the panel then only shows the hardware stats.
- WayVR 26.8 with `wayvrctl` (included in the `wayvr` package). For the graphs: the patched WayVR from [`wayvr-pkg`](wayvr-pkg) (see [Graphs](#graphs)).
- Python 3 (standard library only) and a systemd user session.
- Optional: VRCX, for the VRChat world time.

## Install

### 1. The stats script

From this folder:

```sh
install -Dm755 wayvr-stats ~/.local/bin/wayvr-stats
install -Dm644 -t ~/.config/systemd/user wayvr-stats.path wayvr-stats.service
systemctl --user daemon-reload
systemctl --user enable --now wayvr-stats.path
```

The stats start by themselves whenever WayVR starts, however it is launched, and follow WayVR across restarts and crashes (the service also restarts the script if it fails).

### 2. The watch

Pick one. The files are in this folder and attached to each [release](https://github.com/Seventh-Void/WiVRn-QPro-OSC/releases).

**A. Replace your watch** with a complete one: `watch.xml` (white) or `watch-purple.xml` (purple). Back up your own first:

```sh
cp ~/.config/wayvr/theme/gui/watch.xml ~/.config/wayvr/theme/gui/watch.xml.bak 2>/dev/null
install -Dm644 watch.xml ~/.config/wayvr/theme/gui/watch.xml   # or watch-purple.xml
```

**B. Keep your watch and add the stats to it.** Copy the add-ons next to your `watch.xml`:

```sh
install -Dm644 -t ~/.config/wayvr/theme/gui fpsvr-stats.xml fpsvr-clock.xml
```

then add one line for each to your `~/.config/wayvr/theme/gui/watch.xml`, inside `<elements>`:

```xml
<include src="fpsvr-stats.xml" />   <!-- the stats panel, e.g. below the bottom button row -->
<include src="fpsvr-clock.xml" />   <!-- game and world time, e.g. right under the clock's date row -->
```

The add-ons bring their own colours, templates and buttons (all named `fv_*`, so they don't clash with your watch). Their colours are the `fv_color_*` variables at the top of each file (white by default).

Then restart WayVR (`wayvr --replace`, or close and start it) so it loads the new watch.

The clock is WayVR's own and shows your system's local time zone; the date follows your locale, and 12- or 24-hour time follows WayVR's `clock_12h` setting.

## Graphs

The frame time graphs are **off by default**, because stock WayVR 26.8.0 can't show them for long:

- It never frees the atlas it keeps rendered images and text in, so every new graph image stays there until WayVR exits with `Atlas full` (after about 22 minutes of graphs). [`glyph-atlas-trim.patch`](wayvr-pkg/glyph-atlas-trim.patch) lets the atlas reuse space from images that are no longer drawn, once per frame after all overlays finished rendering. Tested with 400 000 graph images (3.7 h of graphs) without problems.
- Unrelated to the graphs, WayVR 26.8.0 can crash ("Wrong type to unwrap") when a screen share connects at once from a saved PipeWire token; [`screencast-status-crash.patch`](wayvr-pkg/screencast-status-crash.patch) fixes that too.

Install the patched WayVR (`wayvr 26.8.0-1.2`): download `wayvr-26.8.0-1.2-x86_64.pkg.tar.zst` from the [release](https://github.com/Seventh-Void/WiVRn-QPro-OSC/releases) and `sudo pacman -U` it, or build it from [`wayvr-pkg`](wayvr-pkg) (the AUR `wayvr` 26.8.0 package plus both patches):

```sh
cd wayvr-pkg
makepkg -si
```

Then turn the graphs on:

```sh
systemctl --user edit wayvr-stats.service
```

```ini
[Service]
Environment=WAYVR_STATS_GRAPHS=1
```

Without the patched WayVR, everything except the graphs works.

## Settings

All in the service (`systemctl --user edit wayvr-stats.service`, then restart WayVR or `systemctl --user restart wayvr-stats.service`):

| Variable | Default | |
|---|---|---|
| `WAYVR_STATS_GRAPHS` | off | `1` turns the graphs on (patched WayVR only). |
| `WAYVR_STATS_COLOR` | `#FFFFFF` | Colour of the dashed frame budget line. |
| `WAYVR_STATS_TEXT` | `#FFFFFF` | Normal colour of FPS / Avg / ms (match your watch's text colour; `#F4ECFF` for `watch-purple.xml`). |

The rest of the watch colours are the theme variables at the top of `watch.xml`.

## Cost

Measured in VRChat on a 16-core CPU: `wayvr-stats` and its `wayvrctl` connection use about 0.9 % of one core and 20 MB of RAM, and drawing the graphs adds about 0.9 % of one core to WayVR (about 0.1 % of the whole CPU in total).

## Reporting problems

If WayVR crashes, `wayvr-stats` saves its log for you. WayVR itself only logs to `/tmp/wayvr.log`, which it wipes on every start, so the script copies it as soon as a WayVR exits:

- `~/.local/state/wayvr-stats/last-wayvr.log`: the log of the last WayVR that exited, crashed or not.
- `~/.local/state/wayvr-stats/crash-<date>-<time>.log`: kept when that log shows a crash (a panic or `Atlas full`), with the `wayvr` / `wivrn-server` versions and kernel at the top. The 10 newest are kept.

The script's own log: `journalctl --user -u wayvr-stats.service -b`.

Open an issue with the [WayVR watch stats form](https://github.com/Seventh-Void/WiVRn-QPro-OSC/issues/new?template=wayvr_stats.yml) and attach the newest crash log.

## How it works

`wivrn-server` writes each frame's CPU and GPU time for the focused game into a ring buffer at `/dev/shm/wivrn-frametime` (layout documented in [`server/driver/app_pacer.cpp`](../../server/driver/app_pacer.cpp)). `wayvr-stats` reads it 15 times a second, draws the graphs as small SVG files in `$XDG_RUNTIME_DIR` and sends them, the labels and their colours to the watch through one `wayvrctl batch` connection. Labels and colours are only resent when they change.

The Hide/Show buttons use WayVR's `::ElementSetDisplay`, and `::ShellExec` to create or remove `$XDG_RUNTIME_DIR/wayvr-stats.hidden`, which pauses the script.

`wayvr-stats.path` starts the script whenever WayVR writes `$XDG_RUNTIME_DIR/wayvr.pid` (on every start). The script exits when no WayVR is left.
