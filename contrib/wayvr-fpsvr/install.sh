#!/bin/sh
# Installs the WayVR watch stats for the current user (no root needed).
#   ./install.sh          white watch (replaces yours, the old one is backed up)
#   ./install.sh purple   purple watch
#   ./install.sh keep     keep your own watch; copies the fpsvr-*.xml add-ons next to it
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
command -v wayvr >/dev/null || echo "Note: wayvr is not installed yet. Install the patched WayVR too (see the README)."
echo "Done. Restart WayVR (wayvr --replace) to load the watch."
