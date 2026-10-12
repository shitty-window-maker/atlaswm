#!/bin/sh
# setup-transparency.sh — kitty transparency + picom blur + restart
set -e

echo "==> kitty transparency"
KITTY=~/.config/kitty/kitty.conf

# strip old lines if present, then append fresh ones
sed -i '/^background_opacity/d' "$KITTY" 2>/dev/null || true
sed -i '/^background_blur/d' "$KITTY" 2>/dev/null || true
cat >> "$KITTY" << 'EOF'

# ── transparency ───────────────────────────────────────────────
background_opacity   0.85
background_blur      8
dynamic_background_opacity yes
EOF
echo "    kitty.conf updated"

echo "==> picom config"
cat > ~/.config/picom/picom.conf << 'EOF'
backend = "glx";
vsync = true;
use-damage = true;

# ── animations (picom-ftlabs) ──────────────────────────────────
animations = true;
animation-for-open-window    = "zoom";
animation-for-unmap-window   = "slide-out";
animation-for-transient-window = "zoom";
animation-stiffness = 400;
animation-window-mass = 0.5;
animation-damping = 30;
animation-open-exclude = [ "class_g = 'atlas-panel'" ];

# ── shadows ────────────────────────────────────────────────────
shadow = true;
shadow-radius = 12;
shadow-opacity = 0.45;
shadow-offset-x = -6;
shadow-offset-y = -6;
shadow-color = "#000000";
shadow-exclude = [
    "class_g = 'atlas-panel'",
    "override_redirect = true",
];

# ── blur ───────────────────────────────────────────────────────
blur-background = true;
blur-method = "dual_kawase";
blur-strength = 5;
blur-background-frame = false;
blur-background-fixed = false;
blur-exclude = [
    "class_g = 'atlas-panel'",
    "override_redirect = true",
];

# ── fading ─────────────────────────────────────────────────────
fading = true;
fade-in-step  = 0.05;
fade-out-step = 0.05;
fade-delta    = 10;
fade-exclude  = [ "class_g = 'atlas-panel'" ];

# ── stability ──────────────────────────────────────────────────
unredir-if-possible = false;
detect-transient = true;
detect-client-leader = true;
EOF
echo "    picom.conf updated"

echo "==> restarting picom"
pkill picom 2>/dev/null || true
sleep 0.4
picom --config ~/.config/picom/picom.conf --daemon
sleep 0.6
if pgrep -x picom >/dev/null; then
    echo "    picom running (pid $(pgrep -x picom))"
else
    echo "    !!! picom failed to start — trying gaussian blur fallback"
    sed -i 's/blur-method = "dual_kawase"/blur-method = "gaussian"/' ~/.config/picom/picom.conf
    picom --config ~/.config/picom/picom.conf --daemon
    sleep 0.4
    if pgrep -x picom >/dev/null; then
        echo "    picom running with gaussian blur"
    else
        echo "    !!! picom still fails — check: picom --config ~/.config/picom/picom.conf"
        exit 1
    fi
fi

echo "==> reloading open kitty instances"
# kitty supports remote control if enabled; otherwise just tell the user
if pgrep -x kitty >/dev/null; then
    echo "    kitty is running — press Ctrl+Shift+F5 in each window"
    echo "    (or just open a new one with Super+q)"
else
    echo "    no kitty running — new ones will pick it up automatically"
fi

echo
echo "==> done. open a kitty (Super+q) and you should see:"
echo "      · wallpaper showing through the background"
echo "      · blurred behind the terminal"
echo "      · text still fully crisp"
echo
echo "    tweak transparency: edit ~/.config/kitty/kitty.conf"
echo "      background_opacity 0.75   ← more transparent"
echo "      background_opacity 0.95   ← more solid"
echo "      background_blur    0      ← no blur, still transparent"
