#!/bin/sh
# Installs the WayVR watch stats for the current user (no root needed).
#   ./install.sh          white watch (replaces yours, the old one is backed up)
#   ./install.sh purple   purple watch
#   ./install.sh keep     keep your own watch; copies the fpsvr-*.xml add-ons next to it
# If WayVR is running, it is restarted so the watch loads straight away (not with keep:
# the add-ons still have to be added to your watch.xml first).
set -eu
cd "$(dirname "$0")"

case "${1:-white}" in
	white) watch="watch.xml" ;;
	purple) watch="watch-purple.xml" ;;
	keep) watch= ;;
	*) echo "usage: $0 [white|purple|keep]" >&2; exit 1 ;;
esac

if ! command -v python3 >/dev/null; then
	echo "The stats need Python 3. Install it first (Arch: sudo pacman -S python), then run this again." >&2
	exit 1
fi

units=~/.config/systemd/user
gui=~/.config/wayvr/theme/gui
install -Dm755 wayvr-stats ~/.local/bin/wayvr-stats
install -Dm644 -t "$units" wayvr-stats.path wayvr-stats.service
install -Dm644 -t ~/.config/wayvr/theme/fpsvr fpsvr/*.svg

if [ -z "$watch" ]; then
	install -Dm644 -t "$gui" fpsvr-stats.xml fpsvr-button.xml fpsvr-clock.xml
	echo "Add-ons copied to $gui. Add them to your watch.xml: see https://github.com/Seventh-Void/WiVRn-QPro-OSC/tree/qpro-osc/contrib/wayvr-fpsvr#2-the-watch"
else
	if [ -f "$gui/watch.xml" ] && ! cmp -s "$watch" "$gui/watch.xml"; then
		backup="$gui/watch.xml.$(date +%Y%m%d-%H%M%S).bak"
		cp "$gui/watch.xml" "$backup"
		echo "Your old watch is saved as $backup"
	fi
	install -Dm644 "$watch" "$gui/watch.xml"
fi

# The purple watch uses a lilac text colour for the numbers
dropin="$units/wayvr-stats.service.d/purple.conf"
if [ "$watch" = watch-purple.xml ]; then
	mkdir -p "${dropin%/*}"
	printf '[Service]\nEnvironment=WAYVR_STATS_COLOR=#A855F7\nEnvironment=WAYVR_STATS_TEXT=#F4ECFF\n' >"$dropin"
elif [ -n "$watch" ]; then
	rm -f "$dropin"
fi

systemctl --user daemon-reload
systemctl --user enable --now wayvr-stats.path
systemctl --user try-restart wayvr-stats.service

if ! command -v wayvr >/dev/null; then
	echo "Note: wayvr is not installed yet. Install WayVR too (see the README)."
	echo "Done. The watch loads the first time you start WayVR."
	exit 0
fi
if [ -z "$watch" ]; then
	echo "Done. Once the add-ons are in your watch.xml, restart WayVR (wayvr --replace) to load them."
	exit 0
fi

# WayVR running: restart it now so the watch loads straight away. The new WayVR gets the
# same arguments and environment as the running one (it may have been started by WiVRn,
# not from this terminal), and is detached so closing this terminal doesn't close it.
pidfile="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/wayvr.pid"
pid=$(cat "$pidfile" 2>/dev/null || true)
if [ -n "$pid" ] && [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = wayvr ]; then
	echo "WayVR is running, restarting it to load the watch..."
	if python3 - "$pid" "$pidfile" <<-'EOF'
		import os, subprocess, sys, time
		pid, pidfile = sys.argv[1:]
		def nul_list(path):
		    return os.fsdecode(open(path, "rb").read()).split("\0")[:-1]
		env = dict(e.split("=", 1) for e in nul_list(f"/proc/{pid}/environ") if "=" in e)
		args = [a for a in nul_list(f"/proc/{pid}/cmdline")[1:] if a != "--replace"]
		subprocess.Popen(["wayvr", *args, "--replace"], env=env, cwd=os.path.expanduser("~"),
		                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
		                 stderr=subprocess.DEVNULL, start_new_session=True)
		# Wait for the new WayVR to write its pid file
		for _ in range(100):
		    time.sleep(0.1)
		    try:
		        new = open(pidfile).read().strip()
		    except OSError:
		        continue
		    if new and new != pid and os.path.exists(f"/proc/{new}"):
		        sys.exit(0)
		sys.exit(1)
	EOF
	then
		echo "Done. WayVR restarted with the new watch."
	else
		echo "Done, but WayVR didn't come back up by itself. Start WayVR again to load the watch." >&2
	fi
else
	echo "Done. WayVR isn't running; the watch loads the next time you start WayVR."
fi
