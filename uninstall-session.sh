#!/bin/sh
set -e
[ "$(id -u)" -ne 0 ] && exec sudo -E "$0" "$@"

rm -f /usr/share/xsessions/atlas.desktop
rm -rf /usr/local/share/atlas
echo "atlas session removed"
