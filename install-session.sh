#!/bin/sh
# install-session.sh — install atlas as a LightDM session
# requires sudo for /usr/share/xsessions and /usr/local/share/atlas
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    exec sudo -E "$0" "$@"
fi

echo "==> installing to /usr/local/share/atlas"
mkdir -p /usr/local/share/atlas
install -m 0755 "$DIR/atlas"         /usr/local/share/atlas/atlas
install -m 0755 "$DIR/atlas-panel"   /usr/local/share/atlas/atlas-panel
install -m 0755 "$DIR/atlas-ctl"     /usr/local/share/atlas/atlas-ctl
install -m 0755 "$DIR/atlas-session" /usr/local/share/atlas/atlas-session

# update the Exec path in the desktop file (in case DIR differs)
sed "s|^Exec=.*|Exec=/usr/local/share/atlas/atlas-session|" \
    "$DIR/atlas.desktop" > /usr/share/xsessions/atlas.desktop
chmod 0644 /usr/share/xsessions/atlas.desktop

echo "==> installed"
echo "    log out, pick 'Atlas' from the session menu, log back in"
echo "    uninstall: $0 --uninstall" 2>/dev/null
