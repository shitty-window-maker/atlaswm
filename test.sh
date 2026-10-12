#!/bin/sh
# atlas nested-X smoke test
DISP=:1
DIR=$(cd "$(dirname "$0")" && pwd)

# ── clean slate ─────────────────────────────────────────────────
pkill -f "Xephyr $DISP" 2>/dev/null
pkill -x atlas 2>/dev/null
pkill -x atlas-panel 2>/dev/null
pkill -x atlas-loop-status 2>/dev/null
sleep 0.8

rm -f /tmp/atlas.sock
rm -f /tmp/.X${DISP#:}-lock 2>/dev/null || sudo rm -f /tmp/.X${DISP#:}-lock 2>/dev/null
rm -f /tmp/.X11-unix/X${DISP#:} 2>/dev/null

# ── build ───────────────────────────────────────────────────────
echo "==> building"
( cd "$DIR" && cc -O2 atlas.c -o atlas -lX11 -lm )       || { echo "atlas build failed"; exit 1; }
( cd "$DIR" && cc -O2 atlas-panel.c -o atlas-panel -lX11 )|| { echo "panel build failed"; exit 1; }
( cd "$DIR" && cc -O2 atlas-ctl.c -o atlas-ctl )          || { echo "ctl build failed"; exit 1; }
echo "    ok"

# ── start Xephyr ────────────────────────────────────────────────
echo "==> starting Xephyr $DISP"
Xephyr $DISP -screen 1280x800 -ac -noreset -title "atlas test" &
XEPHYR_PID=$!

i=0
while [ ! -e /tmp/.X11-unix/X${DISP#:} ]; do
    i=$((i+1))
    if [ $i -gt 50 ]; then
        echo "Xephyr didn't come up"
        kill $XEPHYR_PID 2>/dev/null
        exit 1
    fi
    sleep 0.1
done
sleep 0.4

# ── start atlas ─────────────────────────────────────────────────
echo "==> starting atlas"
DISPLAY=$DISP "$DIR/atlas" 2>/tmp/atlas.log &
ATLAS_PID=$!

# wait for its socket
i=0
while [ ! -S /tmp/atlas.sock ]; do
    i=$((i+1))
    if [ $i -gt 50 ]; then
        echo "atlas never opened its socket"
        cat /tmp/atlas.log
        kill $XEPHYR_PID 2>/dev/null
        exit 1
    fi
    sleep 0.1
done
sleep 0.3

# ── start panel ─────────────────────────────────────────────────
echo "==> starting panel"
DISPLAY=$DISP "$DIR/atlas-panel" 2>/tmp/atlas-panel.log &
PANEL_PID=$!
sleep 0.4

# ── spawn windows ───────────────────────────────────────────────
echo "==> spawning 4 xterms"
for n in one two three four; do
    DISPLAY=$DISP xterm -fa Monospace -fs 12 -title "$n" &
    sleep 0.25
done
sleep 0.4

# ── status ──────────────────────────────────────────────────────
echo
echo "atlas on $DISP"
echo "  Xephyr pid  = $XEPHYR_PID"
echo "  atlas pid   = $ATLAS_PID"
echo "  panel pid   = $PANEL_PID"
echo
echo "  IPC:        $DIR/atlas-ctl status"
echo "              $DIR/atlas-ctl list"
echo "              $DIR/atlas-ctl locate xterm"
echo "              $DIR/atlas-ctl zoom 0.5"
echo "              $DIR/atlas-ctl card"
echo
echo "  status:     DISPLAY=$DISP atlas-status 'hello world'"
echo "  live loop:  DISPLAY=$DISP atlas-loop-status &"
echo
echo "  logs:       tail -f /tmp/atlas.log"
echo "              tail -f /tmp/atlas-panel.log"
echo
echo "  keys"
echo "    Alt+hjkl           pan the map"
echo "    Alt+Shift+hjkl     move focused window"
echo "    Alt+arrows         focus nearest in direction"
echo "    Alt+Tab            cycle focus"
echo "    Alt+= / Alt+-      zoom"
echo "    Alt+a              overview (toggle)"
echo "    Alt+f              fit focused"
echo "    Alt+0              reset view"
echo "    Alt+m              save mark"
echo "    Alt+1..9           goto mark"
echo "    Alt+grave/F12      scratchpad toggle"
echo "    Alt+Return         spawn terminal"
echo "    Alt+Shift+Return   spawn launcher"
echo "    Alt+Shift+c        close focused"
echo "    Alt+Shift+r        reload config"
echo "    Alt+Shift+q        quit atlas"
echo "    wheel              zoom at cursor"
echo "    Alt+LMB            drag window"
echo "    Alt+RMB            resize window"
echo "    Alt+MMB            pan"
echo
echo "  kill:       pkill -f 'Xephyr $DISP'"
echo

wait $XEPHYR_PID

# ── teardown ────────────────────────────────────────────────────
kill $ATLAS_PID $PANEL_PID 2>/dev/null
pkill -x atlas-loop-status 2>/dev/null
rm -f /tmp/atlas.sock
echo "session ended"
