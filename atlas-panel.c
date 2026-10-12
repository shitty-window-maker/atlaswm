/* atlas-panel — status bar + systray for atlas
 * build: cc -O2 atlas-panel.c -o atlas-panel -lX11 -lXrandr
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>

#define MAX_WINS      64
#define MAX_MARKS     32
#define MAX_HITS      256
#define MAX_TRAY      16

/* config */
static int  cfg_height = 28;
static char cfg_label[64]     = "atlas";
static char cfg_clock_fmt[64] = "%H:%M";
static char cfg_status_cmd[256] = "";
static int  cfg_status_sec    = 5;
static char cfg_monitor[64]   = "DP-0";

static unsigned
    COL_PANEL = 0x0a0e15,
    COL_FG    = 0xe8e3d5,
    COL_DIM   = 0x7a8595,
    COL_ACC   = 0xe0a458,
    COL_BORD  = 0x2a3548,
    COL_CHIP  = 0x1a2436,
    COL_CHIP_ON = 0xe0a458,
    COL_OK    = 0x6aaa6a,
    COL_MISS  = 0xc86060;

typedef struct {
    int  idx;
    char cls[64];
    char title[160];
    double px, py;
    int  w, h;
    char rule[128];
} Win;

typedef struct { char name[32]; } Mk;
typedef struct { int x0, x1; char cmd[128]; } Hit;
typedef struct { Window win; int w, h; int active; } TrayIcon;

static Display *dpy;
static int scr, SW, SH;
static int mon_x = 0, mon_y = 0;
static Window win;
static GC gc;
static XFontStruct *font = NULL;

static unsigned long px_panel, px_fg, px_dim, px_acc, px_bord,
                     px_chip, px_chip_on, px_ok, px_miss;

static Win  wins[MAX_WINS];  static int n_wins = 0;
static Mk   marks[MAX_MARKS]; static int n_marks = 0;
static char focused[64] = "";
static int  online = 0;
static char status_buf[512] = "";
static Atom A_STATUS = None;

static Hit  hits[MAX_HITS]; static int n_hits = 0;

/* systray */
static Atom A_TRAY_OPCODE = None;
static Atom A_TRAY_DATA = None;
static TrayIcon tray[MAX_TRAY];
static int n_tray = 0;
static Window tray_host = None;
static int tray_x0 = 0, tray_x1 = 0;

/* ─── helpers ──────────────────────────────────────────── */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
static void unquote(char *s) {
    size_t l = strlen(s);
    if (l >= 2 && s[0] == '"' && s[l-1] == '"') { memmove(s, s+1, l-2); s[l-2] = 0; }
}
static unsigned hexcol(const char *s) {
    if (*s == '#') s++;
    return (unsigned)strtoul(s, NULL, 16);
}

static void cfg_load(void) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    char path[512];
    snprintf(path, sizeof path, "%s/.config/atlas/panel.conf", home);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char sec[64] = "";
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (e) { *e = 0; snprintf(sec, sizeof sec, "%s", trim(p+1)); }
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(p);
        char *v = trim(eq + 1);
        int inq = 0;
        for (char *q = v; *q; q++) {
            if (*q == '"') inq = !inq;
            else if (*q == '#' && !inq) { *q = 0; v = trim(v); break; }
        }
        if (!strcmp(sec, "panel")) {
            if      (!strcmp(k, "height")) cfg_height = atoi(v);
            else if (!strcmp(k, "label"))  { unquote(v); snprintf(cfg_label, 64, "%s", v); }
            else if (!strcmp(k, "primary_monitor")) { unquote(v); snprintf(cfg_monitor, 64, "%s", v); }
        } else if (!strcmp(sec, "clock")) {
            if (!strcmp(k, "format")) { unquote(v); snprintf(cfg_clock_fmt, 64, "%s", v); }
        } else if (!strcmp(sec, "status")) {
            if      (!strcmp(k, "command"))    { unquote(v); snprintf(cfg_status_cmd, 256, "%s", v); }
            else if (!strcmp(k, "status_sec")) cfg_status_sec = atoi(v);
        } else if (!strcmp(sec, "colors")) {
            unquote(v);
            unsigned c = hexcol(v);
            if      (!strcmp(k, "panel"))      COL_PANEL = c;
            else if (!strcmp(k, "fg"))         COL_FG = c;
            else if (!strcmp(k, "dim"))        COL_DIM = c;
            else if (!strcmp(k, "accent"))     COL_ACC = c;
            else if (!strcmp(k, "border"))     COL_BORD = c;
            else if (!strcmp(k, "chip"))       COL_CHIP = c;
            else if (!strcmp(k, "chip_focus")) COL_CHIP_ON = c;
            else if (!strcmp(k, "rule_ok"))    COL_OK = c;
            else if (!strcmp(k, "rule_miss"))  COL_MISS = c;
        }
    }
    fclose(f);
}

static unsigned long alloc_color(unsigned int rgb) {
    XColor c;
    c.red   = ((rgb >> 16) & 0xff) * 257;
    c.green = ((rgb >> 8)  & 0xff) * 257;
    c.blue  = ( rgb        & 0xff) * 257;
    c.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(dpy, DefaultColormap(dpy, scr), &c)) return BlackPixel(dpy, scr);
    return c.pixel;
}
static void refresh_colors(void) {
    px_panel   = alloc_color(COL_PANEL);
    px_fg      = alloc_color(COL_FG);
    px_dim     = alloc_color(COL_DIM);
    px_acc     = alloc_color(COL_ACC);
    px_bord    = alloc_color(COL_BORD);
    px_chip    = alloc_color(COL_CHIP);
    px_chip_on = alloc_color(COL_CHIP_ON);
    px_ok      = alloc_color(COL_OK);
    px_miss    = alloc_color(COL_MISS);
}

/* ─── IPC ─────────────────────────────────────────────── */
static int sock_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, "/tmp/atlas.sock", sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    struct timeval tv = {0, 300000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}
static void send_cmd(const char *cmd) {
    int fd = sock_connect();
    if (fd < 0) return;
    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);
    char b[512]; ssize_t n;
    while ((n = read(fd, b, sizeof b)) > 0) { }
    close(fd);
}
static int query(const char *cmd, char *out, size_t cap) {
    int fd = sock_connect();
    if (fd < 0) return 0;
    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);
    size_t off = 0; ssize_t n;
    while ((n = read(fd, out + off, cap - 1 - off)) > 0) {
        off += n;
        if (off >= cap - 1) break;
    }
    close(fd);
    out[off] = 0;
    return off > 0;
}
static void poll_status(void) {
    char buf[1024];
    if (!query("status", buf, sizeof buf)) { online = 0; return; }
    online = 1;
    focused[0] = 0;
    char *save = NULL;
    for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
        if (!strncmp(l, "focused ", 8))
            snprintf(focused, sizeof focused, "%s", l + 8);
}
static void poll_list(void) {
    char buf[16384];
    if (!query("list", buf, sizeof buf)) { n_wins = 0; return; }
    n_wins = 0;
    char *save = NULL;
    for (char *l = strtok_r(buf, "\n", &save); l && n_wins < MAX_WINS;
         l = strtok_r(NULL, "\n", &save)) {
        char *f[8] = {0}; int nf = 0;
        char *sv2 = NULL;
        for (char *t = strtok_r(l, "\t", &sv2); t && nf < 8;
             t = strtok_r(NULL, "\t", &sv2))
            f[nf++] = t;
        if (nf < 7) continue;
        Win *w = &wins[n_wins++];
        memset(w, 0, sizeof *w);
        w->idx = atoi(f[0]);
        snprintf(w->cls,   sizeof w->cls,   "%s", f[1]);
        snprintf(w->title, sizeof w->title, "%s", f[2]);
        w->px = atof(f[3]);
        w->py = atof(f[4]);
        w->w  = atoi(f[5]);
        w->h  = atoi(f[6]);
        if (nf >= 8) snprintf(w->rule, sizeof w->rule, "%s", f[7]);
    }
}
static void poll_marks(void) {
    char buf[2048];
    if (!query("marks", buf, sizeof buf)) { n_marks = 0; return; }
    n_marks = 0;
    char *save = NULL;
    for (char *l = strtok_r(buf, "\n", &save); l && n_marks < MAX_MARKS;
         l = strtok_r(NULL, "\n", &save))
        if (sscanf(l, "%31[^\t]", marks[n_marks].name) == 1) n_marks++;
}
static void poll_status_prop(void) {
    if (A_STATUS == None) return;
    Atom type; int fmt; unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, RootWindow(dpy, scr), A_STATUS,
                           0, 1024, False, AnyPropertyType,
                           &type, &fmt, &n, &after, &data) == Success && data && n > 0) {
        size_t cap = n < sizeof status_buf - 1 ? n : sizeof status_buf - 1;
        memcpy(status_buf, data, cap);
        status_buf[cap] = 0;
        while (cap > 0 && (status_buf[cap-1] == '\n' || status_buf[cap-1] == '\r'))
            status_buf[--cap] = 0;
        XFree(data);
    }
}
static void poll_status_cmd(void) {
    if (!cfg_status_cmd[0]) return;
    static time_t last = 0;
    time_t now = time(NULL);
    if (now - last < cfg_status_sec) return;
    last = now;
    FILE *p = popen(cfg_status_cmd, "r");
    if (!p) return;
    size_t total = 0, n;
    while ((n = fread(status_buf + total, 1, sizeof status_buf - 1 - total, p)) > 0) {
        total += n;
        if (total >= sizeof status_buf - 1) break;
    }
    pclose(p);
    while (total > 0 && (status_buf[total-1] == '\n' || status_buf[total-1] == '\r'))
        status_buf[--total] = 0;
    status_buf[total] = 0;
}

/* ─── drawing ──────────────────────────────────────────── */
static int tw(const char *s) {
    if (!s || !*s) return 0;
    return font ? XTextWidth(font, s, strlen(s)) : (int)strlen(s) * 6;
}
static void tx(int x, int y, const char *s, unsigned long col) {
    if (!s || !*s) return;
    if (font) XSetFont(dpy, gc, font->fid);
    XSetForeground(dpy, gc, col);
    XDrawString(dpy, win, gc, x, y, s, strlen(s));
}
static void fill(int x, int y, int w, int h, unsigned long col) {
    XSetForeground(dpy, gc, col);
    XFillRectangle(dpy, win, gc, x, y, w, h);
}

static void tray_arrange(void) {
    int x = tray_x0;
    int h = cfg_height - 10;
    int y0 = 5;
    for (int i = 0; i < n_tray; i++) {
        if (!tray[i].active) continue;
        int w = tray[i].w; if (w < 8) w = 20; if (w > 32) w = 32;
        int hh = tray[i].h; if (hh < 8) hh = 20; if (hh > h) hh = h;
        int yy = y0 + (h - hh) / 2;
        XMoveResizeWindow(dpy, tray[i].win, x, yy, w, hh);
        XMapWindow(dpy, tray[i].win);
        XRaiseWindow(dpy, tray[i].win);
        x += w + 4;
        if (x >= tray_x1 - 20) break;
    }
}

static void draw(void) {
    fill(0, 0, SW, cfg_height, px_panel);
    fill(0, 0, SW, 2, px_acc);

    if (font) XSetFont(dpy, gc, font->fid);
    int base = cfg_height / 2 + (font ? font->ascent / 2 : 4);

    n_hits = 0;
    int x = 12;

    tx(x, base, cfg_label, px_acc);
    x += tw(cfg_label) + 10;

    char cnt[16]; snprintf(cnt, sizeof cnt, "%d", n_wins);
    tx(x, base, cnt, px_dim);
    x += tw(cnt) + 14;

    int chip_h = cfg_height - 10;
    int chip_y = 5;
    for (int i = 0; i < n_wins; i++) {
        Win *w = &wins[i];
        const char *label = w->cls[0] ? w->cls : (w->title[0] ? w->title : "?");
        char buf[64]; snprintf(buf, sizeof buf, "%.20s", label);
        int is_focus = (focused[0] && !strcmp(w->cls, focused));
        int cw = tw(buf) + 16;
        int cx = x;
        fill(cx, chip_y, cw, chip_h, is_focus ? px_chip_on : px_chip);
        if (w->rule[0] && strcmp(w->rule, "-"))
            fill(cx + cw - 3, chip_y, 3, chip_h, px_ok);
        else
            fill(cx + cw - 3, chip_y, 3, chip_h, px_miss);
        tx(cx + 8, base, buf, is_focus ? 0x0d1219 : px_fg);
        if (n_hits < MAX_HITS) {
            hits[n_hits].x0 = cx; hits[n_hits].x1 = cx + cw;
            snprintf(hits[n_hits].cmd, sizeof hits[n_hits].cmd,
                     "focus-idx %d", w->idx);
            n_hits++;
        }
        x += cw + 4;
    }

    /* right side, right-aligned: [tray reserve] [marks] [status] [clock] */
    int right_x = SW - 12;

    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char clk[64];
    strftime(clk, sizeof clk, cfg_clock_fmt, &tm);
    right_x -= tw(clk);
    tx(right_x, base, clk, px_fg);
    right_x -= 16;

    if (status_buf[0]) {
        right_x -= tw(status_buf);
        tx(right_x, base, status_buf, px_dim);
        right_x -= 16;
    }

    for (int i = n_marks - 1; i >= 0; i--) {
        int mw = tw(marks[i].name) + 16;
        int mx = right_x - mw;
        fill(mx, chip_y, mw, chip_h, px_chip);
        tx(mx + 8, base, marks[i].name, px_fg);
        if (n_hits < MAX_HITS) {
            hits[n_hits].x0 = mx; hits[n_hits].x1 = mx + mw;
            snprintf(hits[n_hits].cmd, sizeof hits[n_hits].cmd,
                     "mark goto %s", marks[i].name);
            n_hits++;
        }
        right_x = mx - 4;
    }

    /* tray strip occupies the far right — reserve it */
    tray_x1 = SW - 6;
    /* place tray to the right of everything else, but not past the panel edge */
    tray_x0 = right_x - 4;
    if (tray_x0 < 80) tray_x0 = 80;
    tray_arrange();

    XSetForeground(dpy, gc, px_bord);
    XDrawLine(dpy, win, gc, 0, cfg_height - 1, SW, cfg_height - 1);

    XFlush(dpy);
}

/* ─── systray ──────────────────────────────────────────── */
static void tray_remove(int idx) {
    if (idx < 0 || idx >= n_tray) return;
    tray[idx].active = 0;
    for (int i = idx; i < n_tray - 1; i++) tray[i] = tray[i+1];
    n_tray--;
}

static void tray_dock(Window icon) {
    if (n_tray >= MAX_TRAY) return;
    XSelectInput(dpy, icon, StructureNotifyMask | PropertyChangeMask | ExposureMask);
    XSetWindowBorderWidth(dpy, icon, 0);
    XReparentWindow(dpy, icon, win, 0, 0);

    XWindowAttributes wa;
    int w = 20, h = 20;
    if (XGetWindowAttributes(dpy, icon, &wa)) {
        if (wa.width  > 0 && wa.width  < 64) w = wa.width;
        if (wa.height > 0 && wa.height < 64) h = wa.height;
    }
    XSizeHints sh; long sup;
    if (XGetWMNormalHints(dpy, icon, &sh, &sup)) {
        if ((sh.flags & PMinSize) && sh.min_width  > 0 && sh.min_width  < 64) w = sh.min_width;
        if ((sh.flags & PMinSize) && sh.min_height > 0 && sh.min_height < 64) h = sh.min_height;
    }
    tray[n_tray].win = icon;
    tray[n_tray].w = w;
    tray[n_tray].h = h;
    tray[n_tray].active = 1;
    n_tray++;
    tray_arrange();
    fprintf(stderr, "atlas-panel: docked 0x%lx %dx%d\n", (unsigned long)icon, w, h);
}

static void tray_init(void) {
    char sel_name[64];
    snprintf(sel_name, sizeof sel_name, "_NET_SYSTEM_TRAY_S%d", scr);
    Atom sel = XInternAtom(dpy, sel_name, False);
    tray_host = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), -100, -100, 1, 1, 0, 0, 0);
    XSetSelectionOwner(dpy, sel, tray_host, CurrentTime);
    if (XGetSelectionOwner(dpy, sel) != tray_host) {
        fprintf(stderr, "atlas-panel: another systray owns %s\n", sel_name);
        XDestroyWindow(dpy, tray_host);
        tray_host = None;
        return;
    }
    A_TRAY_OPCODE = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    A_TRAY_DATA   = XInternAtom(dpy, "_NET_SYSTEM_TRAY_MESSAGE_DATA", False);
    Atom manager  = XInternAtom(dpy, "MANAGER", False);
    XEvent ev; memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = RootWindow(dpy, scr);
    ev.xclient.message_type = manager;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = sel;
    ev.xclient.data.l[2] = tray_host;
    XSendEvent(dpy, RootWindow(dpy, scr), False, StructureNotifyMask, &ev);
    fprintf(stderr, "atlas-panel: systray owning %s\n", sel_name);
}

static int tray_handle(XEvent *ev) {
    if (ev->type == ClientMessage &&
        ev->xclient.message_type == A_TRAY_OPCODE &&
        ev->xclient.window == tray_host) {
        int op = (int)ev->xclient.data.l[1];
        if (op == 0) {  /* SYSTEM_TRAY_REQUEST_DOCK */
            Window icon = (Window)ev->xclient.data.l[2];
            if (icon != None) tray_dock(icon);
            return 1;
        }
        return 1;
    }
    if (ev->type == DestroyNotify) {
        for (int i = 0; i < n_tray; i++)
            if (tray[i].win == ev->xdestroywindow.window) {
                tray_remove(i); tray_arrange(); draw(); return 1;
            }
    }
    if (ev->type == UnmapNotify) {
        for (int i = 0; i < n_tray; i++)
            if (tray[i].win == ev->xunmap.window) {
                tray_remove(i); tray_arrange(); draw(); return 1;
            }
    }
    if (ev->type == ConfigureRequest) {
        for (int i = 0; i < n_tray; i++)
            if (tray[i].win == ev->xconfigurerequest.window) {
                if (ev->xconfigurerequest.value_mask & CWWidth)
                    tray[i].w = ev->xconfigurerequest.width;
                if (ev->xconfigurerequest.value_mask & CWHeight)
                    tray[i].h = ev->xconfigurerequest.height;
                tray_arrange();
                return 1;
            }
    }
    return 0;
}

static void tray_click(XButtonEvent *e) {
    for (int i = 0; i < n_tray; i++) {
        if (tray[i].win != e->window) continue;
        if (e->button == 1 || e->button == 3) {
            /* forward the click to the icon so its menu pops */
            XEvent fwd;
            memset(&fwd, 0, sizeof fwd);
            fwd.xbutton.type = ButtonPress;
            fwd.xbutton.display = dpy;
            fwd.xbutton.window = tray[i].win;
            fwd.xbutton.root = RootWindow(dpy, scr);
            fwd.xbutton.subwindow = None;
            fwd.xbutton.time = CurrentTime;
            fwd.xbutton.x = e->x;
            fwd.xbutton.y = e->y;
            fwd.xbutton.x_root = e->x_root;
            fwd.xbutton.y_root = e->y_root;
            fwd.xbutton.state = e->state;
            fwd.xbutton.button = e->button;
            fwd.xbutton.same_screen = True;
            XSendEvent(dpy, tray[i].win, True, ButtonPressMask, &fwd);
        }
        return;
    }
}

/* ─── panel click ──────────────────────────────────────── */
static void on_click(int x, int y, int button) {
    (void)y;
    for (int i = 0; i < n_hits; i++) {
        if (x >= hits[i].x0 && x < hits[i].x1) {
            if (button == 1) send_cmd(hits[i].cmd);
            else if (button == 3 && !strncmp(hits[i].cmd, "focus-idx ", 10)) {
                char c[64];
                snprintf(c, sizeof c, "close-idx %s", hits[i].cmd + 10);
                send_cmd(c);
            }
            return;
        }
    }
}

static int x_err(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

/* ─── main ─────────────────────────────────────────────── */
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    XSetErrorHandler(x_err);

    cfg_load();

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "atlas-panel: no display\n"); return 1; }
    scr = DefaultScreen(dpy);
    SW = DisplayWidth(dpy, scr);
    SH = DisplayHeight(dpy, scr);

    /* primary monitor detection */
    {
        extern int dummy;
        (void)dummy;
        int ev, er;
        if (XRRQueryExtension(dpy, &ev, &er)) {
            int cnt = 0;
            XRRMonitorInfo *mons = XRRGetMonitors(dpy, RootWindow(dpy, scr), True, &cnt);
            if (mons && cnt > 0) {
                int idx = -1;
                if (cfg_monitor[0]) {
                    for (int i = 0; i < cnt; i++)
                        if (mons[i].name &&
                            !strcmp(XGetAtomName(dpy, mons[i].name), cfg_monitor)) { idx = i; break; }
                    if (idx < 0) {
                        char *end; long v = strtol(cfg_monitor, &end, 10);
                        if (*end == 0 && v >= 0 && v < cnt) idx = v;
                    }
                }
                if (idx < 0) for (int i = 0; i < cnt; i++) if (mons[i].primary) { idx = i; break; }
                if (idx < 0) idx = 0;
                mon_x = mons[idx].x;
                mon_y = mons[idx].y;
                SW = mons[idx].width;
                SH = mons[idx].height;
                XRRFreeMonitors(mons);
            }
        }
    }

    A_STATUS = XInternAtom(dpy, "_ATLAS_STATUS", False);

    refresh_colors();

    static const char *fonts[] = {
        "-misc-fixed-medium-r-normal--13-*-*-*-*-*-*-*",
        "-misc-fixed-medium-r-normal--12-*-*-*-*-*-*-*",
        "fixed", NULL };
    for (int i = 0; fonts[i] && !font; i++) font = XLoadQueryFont(dpy, fonts[i]);

    XSetWindowAttributes a = {
        .override_redirect = True,
        .background_pixel  = px_panel,
        .event_mask        = ExposureMask | ButtonPressMask |
                             SubstructureNotifyMask | StructureNotifyMask,
    };
    win = XCreateWindow(dpy, RootWindow(dpy, scr), mon_x, mon_y + SH - cfg_height,
                        SW, cfg_height, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    XMapRaised(dpy, win);
    gc = XCreateGC(dpy, win, 0, NULL);

    tray_init();

    fprintf(stderr, "atlas-panel: %dx%d at +%d+%d h=%d\n",
            SW, SH, mon_x, mon_y, cfg_height);

    int xfd = ConnectionNumber(dpy);
    int tick = 0;
    for (;;) {
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev);

            /* try tray first — it owns the icon subwindows */
            if (tray_handle(&ev)) continue;

            if (ev.type == Expose) {
                if (ev.xexpose.count == 0) draw();
            } else if (ev.type == ButtonPress) {
                if (ev.xbutton.window != win) tray_click(&ev.xbutton);
                else on_click(ev.xbutton.x, ev.xbutton.y, ev.xbutton.button);
            } else if (ev.type == ReparentNotify) {
                /* an app re-docked itself */
                if (ev.xreparent.parent == win) {
                    int known = 0;
                    for (int i = 0; i < n_tray; i++)
                        if (tray[i].win == ev.xreparent.window) { known = 1; break; }
                    if (!known) tray_dock(ev.xreparent.window);
                }
            }
        }
        if (tick % 12 == 0) {
            poll_status();
            poll_list();
            poll_marks();
            status_buf[0] = 0;
            poll_status_prop();
            if (!status_buf[0]) poll_status_cmd();
            draw();
            XRaiseWindow(dpy, win);
        }
        tick++;
        fd_set fds; FD_ZERO(&fds); FD_SET(xfd, &fds);
        struct timeval tv = {0, 16000};
        select(xfd + 1, &fds, NULL, NULL, &tv);
    }
    return 0;
}
