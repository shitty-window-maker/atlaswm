#!/bin/sh
# setup-full.sh — full atlas session install
# run as yourself, not root (uses sudo where needed)

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
echo "==> atlas full setup"
echo "    dir: $DIR"

# ════════════════════════════════════════════════════════════════
# 1. packages
# ════════════════════════════════════════════════════════════════
echo "==> checking packages"
need_pkg() { pacman -Qi "$1" >/dev/null 2>&1 || { echo "    missing: $1"; return 1; }; return 0; }

MISSING=""
for p in kitty rofi feh picom ttf-jetbrains-mono ttf-jetbrains-mono-nerd xorg-xrandr; do
    need_pkg "$p" || MISSING="$MISSING $p"
done

if [ -n "$MISSING" ]; then
    echo "    installing:$MISSING"
    sudo pacman -S --needed --noconfirm $MISSING
fi

# picom-ftlabs for animations — try AUR helpers
if ! pacman -Qi picom-ftlabs-git >/dev/null 2>&1 && ! pacman -Qi picom-ftlabs >/dev/null 2>&1; then
    if command -v yay >/dev/null; then
        yay -S --needed --noconfirm picom-ftlabs-git || echo "    (picom-ftlabs install failed — using stock picom)"
    elif command -v paru >/dev/null; then
        paru -S --needed --noconfirm picom-ftlabs-git || echo "    (picom-ftlabs install failed — using stock picom)"
    else
        echo "    no AUR helper — stock picom, no animations"
    fi
fi

# ════════════════════════════════════════════════════════════════
# 2. kitty
# ════════════════════════════════════════════════════════════════
echo "==> writing kitty config"
mkdir -p ~/.config/kitty
cat > ~/.config/kitty/kitty.conf << 'EOF'
font_family      JetBrainsMono Nerd Font
font_size        11.5
cursor_shape     block
cursor_blink_interval 0

background            #0d1219
foreground            #e8e3d5
selection_background  #e0a458
selection_foreground  #0d1219
cursor                #e0a458
url_color             #e0a458

window_padding_width  8
hide_window_decorations yes
confirm_os_window_close 0
scrollback_lines      10000

# atlas-ish palette
color0  #0d1219
color1  #c86060
color2  #6aaa6a
color3  #e0a458
color4  #5f9ea0
color5  #a060a0
color6  #4a9aaa
color7  #e8e3d5
color8  #2a3548
color9  #e07070
color10 #7ac47a
color11 #f0b878
color12 #7fb8d8
color13 #c888c8
color14 #6ac0d0
color15 #ffffff
EOF

# ════════════════════════════════════════════════════════════════
# 3. rofi
# ════════════════════════════════════════════════════════════════
echo "==> writing rofi config"
mkdir -p ~/.config/rofi
cat > ~/.config/rofi/config.rasi << 'EOF'
configuration {
    modi:         "drun,run,window";
    show-icons:   false;
    drun-display-format: "{name}";
    font:         "JetBrainsMono Nerd Font 12";
}

* {
    bg:      #0d1219;
    bg-alt:  #1a2436;
    fg:      #e8e3d5;
    fg-dim:  #7a8595;
    accent:  #e0a458;
    border:  #2a3548;

    background-color: @bg;
    text-color:       @fg;
    border:           2px;
    border-color:     @border;
    border-radius:    0;
    padding:          12px;
    font: "JetBrainsMono Nerd Font 12";
}

window {
    width: 500px;
    border: 2px;
    border-color: @border;
}

inputbar {
    background-color: @bg-alt;
    text-color: @fg;
    padding: 8px 12px;
    children: [ prompt, textbox-prompt-colon, entry ];
}

prompt {
    background-color: @accent;
    text-color: @bg;
    padding: 0 8px;
}

entry {
    background-color: @bg-alt;
    text-color: @fg;
    placeholder: "search...";
    placeholder-color: @fg-dim;
}

listview {
    lines: 8;
    background-color: @bg;
    padding: 6px 0;
}

element {
    padding: 6px 12px;
    text-color: @fg;
}

element selected {
    background-color: @accent;
    text-color: @bg;
}

element-text {
    background-color: inherit;
    text-color: inherit;
}
EOF

# ════════════════════════════════════════════════════════════════
# 4. picom
# ════════════════════════════════════════════════════════════════
echo "==> writing picom config"
mkdir -p ~/.config/picom
cat > ~/.config/picom/picom.conf << 'EOF'
backend = "glx";
vsync = true;

use-damage = true;
glx-no-stencil = true;
glx-no-rebind-pixmap = true;

# ── animations (picom-ftlabs / picom-simpleanims) ──────────────
animations = true;
animation-for-open-window = "zoom";
animation-for-unmap-window = "fade";
animation-for-transient-window = "fade";
animation-stiffness = 400;
animation-window-mass = 0.5;
animation-damping = 30;
animation-clamping = false;
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

# ── fading ─────────────────────────────────────────────────────
fading = true;
fade-in-step = 0.05;
fade-out-step = 0.05;
fade-delta = 10;
fade-exclude = [ "class_g = 'atlas-panel'" ];

# ── stability ──────────────────────────────────────────────────
unredir-if-possible = false;
detect-transient = true;
detect-client-leader = true;

# ── uncomment for translucent kitty when unfocused ─────────────
# opacity-rule = [
#     "100:class_g = 'kitty' && focused",
#     "85:class_g = 'kitty'",
# ];
EOF

# ════════════════════════════════════════════════════════════════
# 5. atlas config
# ════════════════════════════════════════════════════════════════
echo "==> writing atlas config"
mkdir -p ~/.config/atlas
cat > ~/.config/atlas/config << 'EOF'
[general]
border_size        = 1
card_threshold_pct = 95
zoom_min_pct       = 15
zoom_max_pct       = 100
primary_monitor    = "DP-0"

[general2]
scratch_class = "kitty"

[spring]
pan_k  = 60.0
zoom_k = 60.0

[colors]
bg          = "#0d1219"
card        = "#1a2436"
card_border = "#2e3d54"
focus       = "#e0a458"
fg          = "#e8e3d5"
dim         = "#7a8595"
border      = "#2a3548"

[spawn]
terminal = "kitty"
launcher = "rofi -show drun"

[binds]
"Super+Return"       = "spawn terminal"
"Super+q"            = "spawn terminal"
"Super+p"            = "spawn launcher"
"Super+Shift+Return" = "spawn launcher"

"Super+h"  = "pan left"
"Super+l"  = "pan right"
"Super+k"  = "pan up"
"Super+j"  = "pan down"

"Super+Shift+h" = "move left"
"Super+Shift+l" = "move right"
"Super+Shift+k" = "move up"
"Super+Shift+j" = "move down"

"Super+Left"       = "focus left"
"Super+Right"      = "focus right"
"Super+Up"         = "focus up"
"Super+Down"       = "focus down"
"Super+Tab"        = "cycle next"
"Super+Shift+Tab"  = "cycle prev"

"Super+equal"  = "zoom in"
"Super+minus"  = "zoom out"
"Super+0"      = "reset"
"Super+a"      = "card toggle"
"Super+f"      = "fit focused"

"Super+m"  = "mark save"
"Super+1"  = "mark goto 1"
"Super+2"  = "mark goto 2"
"Super+3"  = "mark goto 3"
"Super+4"  = "mark goto 4"
"Super+5"  = "mark goto 5"
"Super+6"  = "mark goto 6"
"Super+7"  = "mark goto 7"
"Super+8"  = "mark goto 8"
"Super+9"  = "mark goto 9"

"Super+grave" = "scratchpad toggle"
"Super+F12"   = "scratchpad toggle"

"Super+Shift+c" = "close"
"Super+Shift+q" = "quit"
"Super+Shift+r" = "reload"

[rules]
# "firefox"    = "mark:web"
# "kitty"      = "mark:code"

[marks]

[autostart]
xrandr --output HDMI-0 --off
xrandr --output DP-0 --mode 1920x1080 --rate 240.00 --primary
feh --bg-fill /home/spazmatizm/Downloads/cosmic-phenomena-3840x2160-20403.jpg
picom --config ~/.config/picom/picom.conf --daemon
EOF

# ════════════════════════════════════════════════════════════════
# 6. atlas panel config
# ════════════════════════════════════════════════════════════════
echo "==> writing panel config"
cat > ~/.config/atlas/panel.conf << 'EOF'
[panel]
height = 28
primary_monitor = "DP-0"
label = "atlas"

[clock]
format = "%H:%M"

[status]
command = ""
status_sec = 5

[colors]
panel      = "#0a0e15"
fg         = "#e8e3d5"
dim        = "#7a8595"
accent     = "#e0a458"
border     = "#2a3548"
chip       = "#1a2436"
chip_focus = "#e0a458"
rule_ok    = "#6aaa6a"
rule_miss  = "#c86060"
EOF

# ════════════════════════════════════════════════════════════════
# 7. atlas-status helper + live loop
# ════════════════════════════════════════════════════════════════
echo "==> installing atlas-status"
mkdir -p ~/.local/bin

cat > ~/.local/bin/atlas-status << 'EOF'
#!/bin/sh
if [ $# -eq 0 ]; then
    xprop -root -remove _ATLAS_STATUS
else
    xprop -root -f _ATLAS_STATUS 8s -set _ATLAS_STATUS "$*"
fi
EOF
chmod +x ~/.local/bin/atlas-status

cat > ~/.local/bin/atlas-loop-status << 'EOF'
#!/bin/sh
while :; do
    cpu=$(awk '/^cpu /{u=$2+$4; t=$2+$3+$4+$5; printf "%d%%", 100*u/t}' /proc/stat)
    mem=$(awk '/MemTotal/{t=$2} /MemAvailable/{a=$2} END{printf "%d%%", 100*(t-a)/t}' /proc/meminfo)
    bat=$(cat /sys/class/power_supply/BAT0/capacity 2>/dev/null)
    if [ -n "$bat" ]; then
        atlas-status "CPU $cpu  MEM $mem  BAT $bat%"
    else
        atlas-status "CPU $cpu  MEM $mem"
    fi
    sleep 2
done
EOF
chmod +x ~/.local/bin/atlas-loop-status

# ════════════════════════════════════════════════════════════════
# 8. mirror + extended helpers
# ════════════════════════════════════════════════════════════════
echo "==> installing monitor layout helpers"

cat > ~/.local/bin/atlas-mirror << 'EOF'
#!/bin/sh
# DP-0 at 240 primary, HDMI-0 mirrored (60, downscaled)
xrandr --output DP-0 --mode 1920x1080 --rate 240.00 --primary --pos 0x0 \
       --output HDMI-0 --mode 1920x1080 --rate 60.00 --same-as DP-0
EOF
chmod +x ~/.local/bin/atlas-mirror

cat > ~/.local/bin/atlas-single << 'EOF'
#!/bin/sh
# DP-0 at 240 alone
xrandr --output HDMI-0 --off
xrandr --output DP-0 --mode 1920x1080 --rate 240.00 --primary
EOF
chmod +x ~/.local/bin/atlas-single

cat > ~/.local/bin/atlas-dual << 'EOF'
#!/bin/sh
# DP-0 240 on left, HDMI-0 60 on right
xrandr --output DP-0  --mode 1920x1080 --rate 240.00 --primary --pos 0x0 \
       --output HDMI-0 --mode 1920x1080 --rate 60.00  --pos 1920x0
EOF
chmod +x ~/.local/bin/atlas-dual

# ════════════════════════════════════════════════════════════════
# 9. reinstall session
# ════════════════════════════════════════════════════════════════
echo "==> reinstalling atlas session"
sudo "$DIR/install-session.sh"

# ════════════════════════════════════════════════════════════════
# done
# ════════════════════════════════════════════════════════════════
echo
echo "════════════════════════════════════════════════"
echo "  setup complete"
echo "════════════════════════════════════════════════"
echo
echo "  keyboard (Super = Windows key)"
echo "    Super+Return      kitty"
echo "    Super+q           kitty"
echo "    Super+p           rofi"
echo "    Super+a           overview (cards)"
echo "    Super+hjkl        pan"
echo "    Super+Shift+hjkl  move window on plane"
echo "    Super+arrows      focus nearest"
echo "    Super+Tab         cycle focus"
echo "    Super+= / Super+- zoom"
echo "    Super+0           reset view"
echo "    Super+f           fit focused"
echo "    Super+m           save mark"
echo "    Super+1..9        goto mark"
echo "    Super+grave/F12   scratchpad"
echo "    Super+Shift+c/q/r close / quit / reload"
echo
echo "  mouse"
echo "    Super+LMB         drag window"
echo "    Super+RMB         resize window"
echo "    Super+MMB         pan the map"
echo "    wheel             zoom at cursor"
echo
echo "  monitor layouts"
echo "    atlas-single      DP-0 240Hz alone"
echo "    atlas-mirror      DP-0 240Hz + HDMI-0 mirrored"
echo "    atlas-dual        DP-0 240Hz + HDMI-0 extended"
echo
echo "  status"
echo "    atlas-status 'text'"
echo "    atlas-loop-status &"
echo
echo "  to apply:  log out, pick Atlas at LightDM, log back in"
echo
