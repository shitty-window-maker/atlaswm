# atlas

A window manager with a map. Windows live on an infinite 2D plane. Pan it, zoom it, bookmark the places you work.

## what makes it different

On a tiling WM, every window is a slot. You don't decide where things go, the layout does. Atlas gives every window coordinates. It's an infinite plane you move a camera over, not a list of workspaces you switch between.

Two things fall out of that:

- Overview by zooming out. Super+a crossfades the desktop into a map of labeled cards. Type to filter. Enter to fly.
- Marks. Super+m bookmarks the current camera position. Super+1..9 jumps back. Your terminal lives at mark 1. Firefox at mark 2.

A tree can't do either of these, because a tree doesn't have space.

## install

    cc -O2 atlas.c       -o atlas       -lX11 -lXrandr -lm
    cc -O2 atlas-panel.c -o atlas-panel -lX11 -lXrandr
    cc -O2 atlas-ctl.c   -o atlas-ctl

    sudo ./install-session.sh

Log out, pick Atlas at LightDM, log back in.

## three binaries

| binary | what it is |
|---|---|
| atlas | the window manager |
| atlas-panel | taskbar, mark chips, scriptable status, systray |
| atlas-ctl | CLI to a running atlas (/tmp/atlas.sock) |

## keybinds

Super = Windows key. All binds live in ~/.config/atlas/config.

| key | action |
|---|---|
| Super+Return / Super+q | terminal |
| Super+p | launcher |
| Super+a | overview |
| Super+hjkl | pan camera |
| Super+Shift+hjkl | move window on plane |
| Super+arrows | focus nearest |
| Super+Tab | cycle focus |
| Super+= / Super+- | zoom |
| Super+0 | reset view |
| Super+f | fit focused |
| Super+m | save mark |
| Super+1..9 | goto mark |
| Super+grave | scratchpad |
| Super+Shift+c | close |
| Super+Shift+r | reload config |
| Super+Shift+q | quit |
| Super+LMB | drag window |
| Super+RMB | resize |
| Super+MMB | pan |
| wheel | zoom at cursor |

## config

~/.config/atlas/config, written on first run.

    [general]
    border_size     = 1
    primary_monitor = "DP-0"

    [colors]
    bg    = "#0d1219"
    focus = "#e0a458"
    fg    = "#e8e3d5"

    [spawn]
    terminal = "kitty"
    launcher = "rofi -show drun"

    [rules]
    "firefox" = "mark:web"
    "kitty"   = "mark:code"

    [marks]
    code = 0,0
    web  = 1600,-400

## atlas-ctl

    atlas-ctl status
    atlas-ctl list
    atlas-ctl locate kitty
    atlas-ctl zoom 0.5
    atlas-ctl card
    atlas-ctl mark save home
    atlas-ctl mark goto home

## design notes

- Windows keep their place, not their slot. Nothing reflows unless you move it.
- Real windows never resize during pan. Only the card overlay scales.
- Card mode unmaps clients. The overlay draws on an empty root.
- Panel is a separate process. It talks to atlas over a Unix socket. If it crashes, the WM lives.
- Config reloads live. Super+Shift+r reparses the file. No restart.

## license

MIT.
