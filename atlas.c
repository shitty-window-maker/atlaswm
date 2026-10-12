/* atlas — a window manager with a map.
 * build: cc -O2 atlas.c -o atlas -lX11
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

typedef struct Client {
    Window  win;
    double  px, py;
    int     w, h;
    char    title[256];
    char    cls[64];
    char    rule[128];
    int     hidden_by_wm;
    struct Client *next;
} Client;

typedef struct { char key[64]; char act[256]; } Bind;
typedef struct { char name[32]; double x, y; } Mark;
typedef struct { char match[64]; char act[128]; } Rule;

static Display *dpy;
static int      scr, SW, SH;
static int      mon_x = 0, mon_y = 0;
static char     cfg_monitor[64] = "";    /* primary monitor top-left */
static Window   root;
static GC       gc;

static Client  *clients = NULL;
static Client  *focused = NULL;

static double   view_cx = 0, view_cy = 0, view_zoom = 1.0;
static double   tgt_cx  = 0, tgt_cy  = 0, tgt_zoom  = 1.0;
static double   vel_cx  = 0, vel_cy  = 0, vel_zoom  = 0;
static int      anim_active = 0;

static Window   card_win = None;
static int      in_card_mode = 0;

static char     search_buf[64] = "";
static int      search_len = 0;

static Atom     A_WMNAME, A_UTF8, A_ATLAS_X, A_ATLAS_Y;
static int      running = 1;
static int      ipc_fd  = -1;

static int      border_w       = 1;
static double   card_threshold = 0.95;
static double   zoom_min       = 0.15;
static double   zoom_max       = 1.00;
static double   spring_pan_k   = 60.0;
static double   spring_zoom_k  = 60.0;

static unsigned C_BG          = 0x0d1219;
static unsigned C_CARD        = 0x1a2436;
static unsigned C_CARD_BORDER = 0x2e3d54;
static unsigned C_FOCUS       = 0xe0a458;
static unsigned C_FG          = 0xe8e3d5;
static unsigned C_DIM         = 0x7a8595;
static unsigned C_BORDER      = 0x2a3548;

static char     spawn_terminal[256] = "xterm -fa Monospace -fs 12";
static char     spawn_launcher[256] = "dmenu_run";

static Bind     binds[128];  static int n_binds = 0;
static Mark     marks[32];   static int n_marks = 0;
static Rule     rules[64];   static int n_rules = 0;

static Client  *scratch_c = NULL;
static int      scratch_hidden = 0;
static int      scratch_pending = 0;
static char     scratch_class[64] = "xterm";
static char     session_path[512] = "";
static char     cfg_path[512];
static char     mark_path[512];

static Client  *drag_c   = NULL;
static int      drag_mode = 0;
static int      drag_sx, drag_sy;
static double   drag_opx, drag_opy;
static int      drag_ow,  drag_oh;

static unsigned long alloc_color(unsigned int rgb) {
    static struct { unsigned int rgb; unsigned long px; } cache[64];
    static int n = 0;
    for (int i = 0; i < n; i++) if (cache[i].rgb == rgb) return cache[i].px;
    XColor c;
    c.red   = ((rgb >> 16) & 0xff) * 257;
    c.green = ((rgb >> 8)  & 0xff) * 257;
    c.blue  = ( rgb        & 0xff) * 257;
    c.flags = DoRed | DoGreen | DoBlue;
    unsigned long px;
    if (XAllocColor(dpy, DefaultColormap(dpy, scr), &c)) px = c.pixel;
    else                                                  px = BlackPixel(dpy, scr);
    if (n < 64) { cache[n].rgb = rgb; cache[n].px = px; n++; }
    return px;
}

static void plane_to_screen(double px, double py, int *sx, int *sy) {
    *sx = (int)((px - view_cx) * view_zoom + SW / 2.0);
    *sy = (int)((py - view_cy) * view_zoom + SH / 2.0);
}
static void screen_to_plane(int sx, int sy, double *px, double *py) {
    *px = view_cx + (sx - mon_x - SW / 2.0) / view_zoom;
    *py = view_cy + (sy - mon_y - SH / 2.0) / view_zoom;
}

static void update_title(Client *c) {
    c->title[0] = 0;
    Atom type; int fmt; unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, c->win, A_WMNAME, 0, 256, False, A_UTF8,
                           &type, &fmt, &n, &after, &data) == Success && data) {
        snprintf(c->title, sizeof c->title, "%.*s", (int)n, data);
        XFree(data); return;
    }
    char *name = NULL;
    if (XFetchName(dpy, c->win, &name) && name) {
        snprintf(c->title, sizeof c->title, "%s", name);
        XFree(name);
    }
}
static void update_class(Client *c) {
    c->cls[0] = 0;
    XClassHint ch;
    if (XGetClassHint(dpy, c->win, &ch)) {
        if (ch.res_class) snprintf(c->cls, sizeof c->cls, "%s", ch.res_class);
        if (ch.res_name)  XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
    }
}

static void anim_to(double cx, double cy, double z) {
    tgt_cx = cx; tgt_cy = cy;
    if (z < zoom_min) z = zoom_min;
    if (z > zoom_max) z = zoom_max;
    tgt_zoom = z;
    anim_active = 1;
}
static void anim_kill(void) { anim_active = 0; vel_cx = vel_cy = vel_zoom = 0; }

static int matches_search(Client *c) {
    if (search_len == 0) return 1;
    char needle[64];
    for (int i = 0; i < search_len; i++)
        needle[i] = (char)tolower((unsigned char)search_buf[i]);
    needle[search_len] = 0;
    char hay[400];
    snprintf(hay, sizeof hay, "%s %s", c->title, c->cls);
    for (char *p = hay; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strstr(hay, needle) != NULL;
}
static Client *first_match(void) {
    for (Client *c = clients; c; c = c->next)
        if (matches_search(c)) return c;
    return NULL;
}

static void draw_cards(void) {
    if (card_win == None) return;

    XSetForeground(dpy, gc, alloc_color(C_BG));
    XFillRectangle(dpy, card_win, gc, 0, 0, SW, SH);

    unsigned long card_px    = alloc_color(C_CARD);
    unsigned long bord_px    = alloc_color(C_CARD_BORDER);
    unsigned long focus_px   = alloc_color(C_FOCUS);
    unsigned long focus_dim  = alloc_color(0x8a6a3a);
    unsigned long fg_px      = alloc_color(C_FG);
    unsigned long dim_px     = alloc_color(C_DIM);
    unsigned long shadow_px  = alloc_color(0x050810);
    unsigned long hi_px      = alloc_color(0x2c3a52);
    unsigned long bar_dim    = alloc_color(0x3a4a5f);
    unsigned long rule_ok    = alloc_color(0x6aaa6a);
    unsigned long rule_miss  = alloc_color(0xc86060);

    for (Client *c = clients; c; c = c->next) {
        int sx, sy;
        plane_to_screen(c->px, c->py, &sx, &sy);
        int w = (int)(c->w * view_zoom);
        int h = (int)(c->h * view_zoom);
        if (w < 6) w = 6;
        if (h < 6) h = 6;
        int x = sx - w / 2;
        int y = sy - h / 2;
        if (x + w < 0 || x > SW || y + h < 0 || y > SH) continue;

        int match  = matches_search(c);
        int is_foc = (c == focused && match);

        unsigned long fill   = !match ? 0x14181e : card_px;
        unsigned long border = !match ? 0x1c2028 : (is_foc ? focus_px : bord_px);
        unsigned long title  = !match ? 0x2c3238 : (is_foc ? focus_px : fg_px);
        unsigned long klass  = !match ? 0x1e242a : dim_px;

        /* shadow behind focused card */
        if (is_foc && w > 30 && h > 20) {
            XSetForeground(dpy, gc, shadow_px);
            XFillRectangle(dpy, card_win, gc, x + 3, y + 3, w, h);
        }

        /* body */
        XSetForeground(dpy, gc, fill);
        XFillRectangle(dpy, card_win, gc, x, y, w, h);

        /* top inner highlight */
        if (match && w > 6 && h > 6) {
            XSetForeground(dpy, gc, hi_px);
            XDrawLine(dpy, card_win, gc, x + 1, y + 1, x + w - 2, y + 1);
        }

        /* left accent bar */
        int bar_w = is_foc ? 4 : 3;
        if (match && w > 8 && h > 4) {
            XSetForeground(dpy, gc, is_foc ? focus_px : bar_dim);
            XFillRectangle(dpy, card_win, gc, x, y, bar_w, h);
        }

        /* border — double for focused */
        if (is_foc && w > 6 && h > 6) {
            XSetForeground(dpy, gc, focus_dim);
            XDrawRectangle(dpy, card_win, gc, x, y, w, h);
            XSetForeground(dpy, gc, focus_px);
            XDrawRectangle(dpy, card_win, gc, x + 1, y + 1, w - 2, h - 2);
        } else {
            XSetForeground(dpy, gc, border);
            XDrawRectangle(dpy, card_win, gc, x, y, w, h);
        }

        /* rule indicator dot, top-right */
        if (match && w > 44 && h > 26) {
            int has_rule = (c->rule[0] && strcmp(c->rule, "-"));
            XSetForeground(dpy, gc, has_rule ? rule_ok : rule_miss);
            XFillRectangle(dpy, card_win, gc, x + w - 9, y + 5, 4, 4);
        }

        /* title + class */
        if (w > 60 && h > 20) {
            int tx    = x + bar_w + 8;
            int avail = w - bar_w - 18;
            int maxc  = avail / 6;
            if (maxc < 0) maxc = 0;

            XSetForeground(dpy, gc, title);
            char buf[160];
            snprintf(buf, sizeof buf, "%.*s", maxc, c->title);
            XDrawString(dpy, card_win, gc, tx, y + 20, buf, (int)strlen(buf));

            if (h > 46 && c->cls[0]) {
                XSetForeground(dpy, gc, klass);
                char cb[80];
                int maxc2 = maxc > 24 ? 24 : maxc;
                snprintf(cb, sizeof cb, "%.*s", maxc2, c->cls);
                XDrawString(dpy, card_win, gc, tx, y + 40, cb, (int)strlen(cb));
            }
        }
    }

    if (search_len > 0) {
        XSetForeground(dpy, gc, fg_px);
        XDrawString(dpy, card_win, gc, 20, SH - 20, search_buf, search_len);
    }

    XFlush(dpy);
}

static void apply_view(void) {
    if (in_card_mode) { draw_cards(); return; }
    for (Client *c = clients; c; c = c->next) {
        int sx, sy;
        plane_to_screen(c->px, c->py, &sx, &sy);
        XMoveResizeWindow(dpy, c->win, sx - c->w/2 + mon_x, sy - c->h/2 + mon_y, c->w, c->h);
        XSetWindowBorder(dpy, c->win,
            alloc_color(c == focused ? C_FOCUS : C_BORDER));
    }
    XFlush(dpy);
}

static void enter_card_mode(void) {
    if (in_card_mode) return;
    in_card_mode = 1;
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    for (Client *c = clients; c; c = c->next) {
        c->hidden_by_wm = 1;
        XUnmapWindow(dpy, c->win);
    }
    XMapRaised(dpy, card_win);
    apply_view();
}
static void enter_real_mode(void) {
    if (!in_card_mode) return;
    in_card_mode = 0;
    XUnmapWindow(dpy, card_win);
    XUngrabKeyboard(dpy, CurrentTime);
    for (Client *c = clients; c; c = c->next) {
        c->hidden_by_wm = 0;
        XMapWindow(dpy, c->win);
    }
    if (focused) XSetInputFocus(dpy, focused->win, RevertToPointerRoot, CurrentTime);
    apply_view();
}

static Client *find(Window w) {
    for (Client *c = clients; c; c = c->next)
        if (c->win == w) return c;
    return NULL;
}
static Client *client_at(int rx, int ry) {
    int sx = rx - mon_x, sy = ry - mon_y;
    for (Client *c = clients; c; c = c->next) {
        int csx, csy;
        plane_to_screen(c->px, c->py, &csx, &csy);
        int sw = (int)(c->w * view_zoom); if (sw < 20) sw = 20;
        int sh = (int)(c->h * view_zoom); if (sh < 20) sh = 20;
        if (sx >= csx - sw/2 && sx < csx + sw/2 &&
            sy >= csy - sh/2 && sy < csy + sh/2)
            return c;
    }
    return NULL;
}
static void save_position(Client *c) {
    if (!c) return;
    long xv = (long)(c->px * 16.0);
    long yv = (long)(c->py * 16.0);
    XChangeProperty(dpy, c->win, A_ATLAS_X, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&xv, 1);
    XChangeProperty(dpy, c->win, A_ATLAS_Y, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&yv, 1);
}
static void set_focus(Client *c) {
    if (focused == c) return;
    focused = c;
    if (c && !in_card_mode)
        XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    apply_view();
}
static void apply_rule(Client *c) {
    for (int i = 0; i < n_rules; i++) {
        if (!strcasestr(c->cls, rules[i].match)) continue;
        const char *act = rules[i].act;
        snprintf(c->rule, sizeof c->rule, "%s", act);
        if (!strncmp(act, "mark:", 5)) {
            for (int j = 0; j < n_marks; j++)
                if (!strcmp(marks[j].name, act + 5)) {
                    c->px = marks[j].x;
                    c->py = marks[j].y;
                    fprintf(stderr, "rule: %s -> mark %s\n", c->cls, act + 5);
                    return;
                }
        } else if (!strncmp(act, "at:", 3)) {
            double x, y;
            if (sscanf(act + 3, "%lf,%lf", &x, &y) == 2) {
                c->px = x; c->py = y;
                fprintf(stderr, "rule: %s -> at %.0f,%.0f\n", c->cls, x, y);
                return;
            }
        } else if (!strcmp(act, "float")) {
            c->px = view_cx + (rand() % 200) - 100;
            c->py = view_cy + (rand() % 200) - 100;
            fprintf(stderr, "rule: %s -> float\n", c->cls);
            return;
        }
    }
}

static void manage(Window w, XWindowAttributes *attr) {
    Client *c = calloc(1, sizeof *c);
    c->win = w;
    c->w = attr->width;
    c->h = attr->height;

    Atom type; int fmt; unsigned long cnt, after;
    unsigned char *data = NULL;
    int restored = 0;
    if (XGetWindowProperty(dpy, w, A_ATLAS_X, 0, 1, False, XA_CARDINAL,
                           &type, &fmt, &cnt, &after, &data) == Success && data && cnt == 1) {
        long xv = *(long *)data; XFree(data); data = NULL;
        if (XGetWindowProperty(dpy, w, A_ATLAS_Y, 0, 1, False, XA_CARDINAL,
                               &type, &fmt, &cnt, &after, &data) == Success && data && cnt == 1) {
            long yv = *(long *)data; XFree(data); data = NULL;
            c->px = (double)xv / 16.0;
            c->py = (double)yv / 16.0;
            restored = 1;
        }
    }
    if (data) XFree(data);
    if (!restored) {
        c->px = view_cx + (rand() % 200) - 100;
        c->py = view_cy + (rand() % 200) - 100;
    }

    update_title(c);
    update_class(c);
    if (scratch_pending && strcasestr(c->cls, scratch_class)) {
        scratch_c = c;
        scratch_pending = 0;
        scratch_hidden = 0;
        fprintf(stderr, "atlas: scratchpad = %s\n", c->cls);
    }
    apply_rule(c);
    c->next = clients;
    clients = c;

    XSetWindowBorderWidth(dpy, w, border_w);
    XSetWindowBorder(dpy, w, alloc_color(C_BORDER));
    XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask | EnterWindowMask);
    XMapWindow(dpy, w);
    save_position(c);
    set_focus(c);
    apply_view();
}
static void unmanage(Client *c) {
    if (!c) return;
    int was_focused = (focused == c);
    Client **p = &clients;
    while (*p && *p != c) p = &(*p)->next;
    if (*p) *p = c->next;
    Client *next_after = (*p) ? *p : clients;
    free(c);
    if (was_focused) {
        focused = NULL;
        if (next_after) {
            focused = next_after;
            if (!in_card_mode)
                XSetInputFocus(dpy, focused->win, RevertToPointerRoot, CurrentTime);
            anim_to(focused->px, focused->py, view_zoom);
        }
    }
    apply_view();
}
static void close_client(Client *c) {
    if (!c) return;
    Atom protos = XInternAtom(dpy, "WM_PROTOCOLS", False);
    Atom del    = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = c->win;
    ev.xclient.message_type = protos;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = del;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, c->win, False, NoEventMask, &ev);
}

static void pan(double dx, double dy) {
    anim_kill();
    view_cx += dx; view_cy += dy;
    apply_view();
}
static void zoom_at(double nz, int sx, int sy) {
    anim_kill();
    if (nz < zoom_min) nz = zoom_min;
    if (nz > zoom_max) nz = zoom_max;
    double px, py;
    screen_to_plane(sx, sy, &px, &py);
    view_zoom = nz;
    view_cx = px - (sx - SW / 2.0) / view_zoom;
    view_cy = py - (sy - SH / 2.0) / view_zoom;
    apply_view();
}
static void fit_all(void) {
    anim_kill();
    if (!clients) { view_cx = view_cy = 0; view_zoom = 1; enter_real_mode(); return; }
    double mnx = 1e18, mny = 1e18, mxx = -1e18, mxy = -1e18;
    for (Client *c = clients; c; c = c->next) {
        double x0 = c->px - c->w / 2.0, x1 = c->px + c->w / 2.0;
        double y0 = c->py - c->h / 2.0, y1 = c->py + c->h / 2.0;
        if (x0 < mnx) mnx = x0;
        if (y0 < mny) mny = y0;
        if (x1 > mxx) mxx = x1;
        if (y1 > mxy) mxy = y1;
    }
    double bw = mxx - mnx, bh = mxy - mny;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    double zx = (SW - 120) / bw, zy = (SH - 120) / bh;
    double z = zx < zy ? zx : zy;
    if (z > zoom_max) z = zoom_max;
    if (z < zoom_min) z = zoom_min;
    view_cx = (mnx + mxx) / 2;
    view_cy = (mny + mxy) / 2;
    view_zoom = z;
    enter_card_mode();
    apply_view();
}
static void reset_view(void) {
    anim_kill();
    view_zoom = 1.0;
    if (focused) { view_cx = focused->px; view_cy = focused->py; }
    else         { view_cx = view_cy = 0; }
    enter_real_mode();
    apply_view();
}
static void reveal(Client *c) { if (c) anim_to(c->px, c->py, view_zoom); }

static void focus_dir(int dir) {
    if (!focused || !clients) return;
    double fx = focused->px, fy = focused->py;
    Client *best = NULL;
    double bs = 1e30;
    for (Client *c = clients; c; c = c->next) {
        if (c == focused) continue;
        double dx = c->px - fx, dy = c->py - fy;
        double primary = 0, perp = 0;
        int ok = 0;
        if      (dir == 0 && dx < 0) { primary = -dx; perp = fabs(dy); ok = 1; }
        else if (dir == 1 && dx > 0) { primary =  dx; perp = fabs(dy); ok = 1; }
        else if (dir == 2 && dy < 0) { primary = -dy; perp = fabs(dx); ok = 1; }
        else if (dir == 3 && dy > 0) { primary =  dy; perp = fabs(dx); ok = 1; }
        if (!ok) continue;
        double s = primary + perp * 2.0;
        if (s < bs) { bs = s; best = c; }
    }
    if (!best) return;
    set_focus(best);
    reveal(best);
}
static void cycle_focus(int dir) {
    if (!clients) return;
    Client *arr[256]; int n = 0;
    for (Client *c = clients; c && n < 256; c = c->next) arr[n++] = c;
    int idx = -1;
    for (int i = 0; i < n; i++) if (arr[i] == focused) { idx = i; break; }
    idx = (idx < 0) ? 0 : (idx + dir + n) % n;
    set_focus(arr[idx]);
    if (focused) reveal(focused);
}

static void start_drag(Client *c, int sx, int sy) {
    if (!c) return;
    drag_c = c; drag_mode = 1;
    drag_sx = sx; drag_sy = sy;
    drag_opx = c->px; drag_opy = c->py;
    drag_ow = c->w; drag_oh = c->h;
    set_focus(c);
    XRaiseWindow(dpy, c->win);
    XGrabPointer(dpy, root, False, PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}
static void start_resize(Client *c, int sx, int sy) {
    if (!c) return;
    drag_c = c; drag_mode = 2;
    drag_sx = sx; drag_sy = sy;
    drag_opx = c->px; drag_opy = c->py;
    drag_ow = c->w; drag_oh = c->h;
    set_focus(c);
    XRaiseWindow(dpy, c->win);
    XGrabPointer(dpy, root, False, PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}

static void marks_load(void) {
    if (!mark_path[0]) return;
    FILE *f = fopen(mark_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || !*p) continue;
        char name[32];
        double x, y;
        if (sscanf(p, "%31[^= ] = %lf , %lf", name, &x, &y) == 3) {
            int idx = -1;
            for (int i = 0; i < n_marks; i++)
                if (!strcmp(marks[i].name, name)) { idx = i; break; }
            if (idx < 0 && n_marks < 32) idx = n_marks++;
            if (idx >= 0) {
                snprintf(marks[idx].name, 32, "%s", name);
                marks[idx].x = x; marks[idx].y = y;
            }
        }
    }
    fclose(f);
}
static void marks_save(void) {
    if (!mark_path[0]) return;
    FILE *f = fopen(mark_path, "w");
    if (!f) return;
    fprintf(f, "# atlas marks\n");
    for (int i = 0; i < n_marks; i++)
        fprintf(f, "%s = %.1f,%.1f\n", marks[i].name, marks[i].x, marks[i].y);
    fclose(f);
}
static int mark_goto(const char *name) {
    for (int i = 0; i < n_marks; i++)
        if (!strcmp(marks[i].name, name)) {
            anim_to(marks[i].x, marks[i].y, view_zoom);
            return 1;
        }
    return 0;
}
static void mark_save(void) {
    char name[32] = "";
    for (int d = 1; d <= 9; d++) {
        char tryn[32]; snprintf(tryn, sizeof tryn, "%d", d);
        int used = 0;
        for (int i = 0; i < n_marks; i++)
            if (!strcmp(marks[i].name, tryn)) { used = 1; break; }
        if (!used) { snprintf(name, sizeof name, "%s", tryn); break; }
    }
    if (!name[0]) snprintf(name, sizeof name, "1");
    int idx = -1;
    for (int i = 0; i < n_marks; i++)
        if (!strcmp(marks[i].name, name)) { idx = i; break; }
    if (idx < 0) { if (n_marks >= 32) return; idx = n_marks++; }
    snprintf(marks[idx].name, 32, "%s", name);
    marks[idx].x = view_cx; marks[idx].y = view_cy;
    marks_save();
    fprintf(stderr, "atlas: saved mark '%s' at (%.0f,%.0f)\n", name, view_cx, view_cy);
}

static void spawn(const char *cmd) {
    if (!cmd || !*cmd) return;
    if (fork() == 0) {
        setsid();
        close(ConnectionNumber(dpy));
        if (ipc_fd >= 0) close(ipc_fd);
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }
}

static const char *DEFAULT_CFG =
"# atlas config\n"
"\n[general]\n"
"border_size        = 1\n"
"card_threshold_pct = 95\n"
"zoom_min_pct       = 15\n"
"zoom_max_pct       = 100\n"
"\n[general2]\n"
"scratch_class = \"xterm\"\n"
"\n[spring]\n"
"pan_k  = 60.0\n"
"zoom_k = 60.0\n"
"\n[colors]\n"
"bg          = \"#0d1219\"\n"
"card        = \"#1a2436\"\n"
"card_border = \"#2e3d54\"\n"
"focus       = \"#e0a458\"\n"
"fg          = \"#e8e3d5\"\n"
"dim         = \"#7a8595\"\n"
"border      = \"#2a3548\"\n"
"\n[spawn]\n"
"terminal = \"xterm -fa Monospace -fs 12\"\n"
"launcher = \"dmenu_run\"\n"
"\n[binds]\n"
"\"Alt+Return\"       = \"spawn terminal\"\n"
"\"Alt+Shift+Return\" = \"spawn launcher\"\n"
"\"Alt+h\" = \"pan left\"\n"
"\"Alt+l\" = \"pan right\"\n"
"\"Alt+k\" = \"pan up\"\n"
"\"Alt+j\" = \"pan down\"\n"
"\"Alt+Shift+h\" = \"move left\"\n"
"\"Alt+Shift+l\" = \"move right\"\n"
"\"Alt+Shift+k\" = \"move up\"\n"
"\"Alt+Shift+j\" = \"move down\"\n"
"\"Alt+Left\"  = \"focus left\"\n"
"\"Alt+Right\" = \"focus right\"\n"
"\"Alt+Up\"    = \"focus up\"\n"
"\"Alt+Down\"  = \"focus down\"\n"
"\"Alt+Tab\"       = \"cycle next\"\n"
"\"Alt+Shift+Tab\" = \"cycle prev\"\n"
"\"Alt+equal\" = \"zoom in\"\n"
"\"Alt+minus\" = \"zoom out\"\n"
"\"Alt+0\"     = \"reset\"\n"
"\"Alt+a\"     = \"card toggle\"\n"
"\"Alt+f\"     = \"fit focused\"\n"
"\"Alt+m\" = \"mark save\"\n"
"\"Alt+grave\" = \"scratchpad toggle\"\n"
"\"Alt+1\" = \"mark goto 1\"\n"
"\"Alt+2\" = \"mark goto 2\"\n"
"\"Alt+3\" = \"mark goto 3\"\n"
"\"Alt+4\" = \"mark goto 4\"\n"
"\"Alt+5\" = \"mark goto 5\"\n"
"\"Alt+6\" = \"mark goto 6\"\n"
"\"Alt+7\" = \"mark goto 7\"\n"
"\"Alt+8\" = \"mark goto 8\"\n"
"\"Alt+9\" = \"mark goto 9\"\n"
"\"Alt+Shift+c\" = \"close\"\n"
"\"Alt+Shift+q\" = \"quit\"\n"
"\"Alt+Shift+r\" = \"reload\"\n"
"\"Alt+F12\" = \"scratchpad toggle\"\n"
"\n[rules]\n"
"# \"firefox\"     = \"mark:web\"\n"
"# \"kitty\"       = \"mark:code\"\n"
"# \"xterm\"       = \"mark:code\"\n"
"# \"discord\"     = \"mark:chat\"\n"
"# \"mpv\"         = \"float\"\n"
"\n[marks]\n";

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
static int parse_hex(const char *s, unsigned *out) {
    if (*s == '#') s++;
    char *end;
    long v = strtol(s, &end, 16);
    if (*end || v < 0) return -1;
    *out = (unsigned)v;
    return 0;
}
static void cfg_mark(const char *line) {
    if (n_marks >= 32) return;
    char name[32] = "";
    double x, y;
    if (sscanf(line, " %31[^= \t] = %lf , %lf", name, &x, &y) == 3 ||
        sscanf(line, " \"%31[^\"]\" = %lf , %lf", name, &x, &y) == 3) {
        snprintf(marks[n_marks].name, 32, "%s", name);
        marks[n_marks].x = x; marks[n_marks].y = y;
        n_marks++;
    }
}
static void cfg_kv(const char *sec, char *k, char *v) {
    if (!strcmp(sec, "general")) {
        if      (!strcmp(k, "primary_monitor")) { unquote(v); snprintf(cfg_monitor, 64, "%s", v); }
        else if (!strcmp(k, "border_size"))        border_w = atoi(v);
        else if (!strcmp(k, "card_threshold_pct")) card_threshold = atoi(v) / 100.0;
        else if (!strcmp(k, "zoom_min_pct"))       zoom_min = atoi(v) / 100.0;
        else if (!strcmp(k, "zoom_max_pct"))       zoom_max = atoi(v) / 100.0;
    } else if (!strcmp(sec, "spring")) {
        if      (!strcmp(k, "pan_k"))  spring_pan_k  = atof(v);
        else if (!strcmp(k, "zoom_k")) spring_zoom_k = atof(v);
    } else if (!strcmp(sec, "colors")) {
        unquote(v);
        unsigned c;
        if (parse_hex(v, &c) == 0) {
            if      (!strcmp(k, "bg"))          C_BG          = c;
            else if (!strcmp(k, "card"))        C_CARD        = c;
            else if (!strcmp(k, "card_border")) C_CARD_BORDER = c;
            else if (!strcmp(k, "focus"))       C_FOCUS       = c;
            else if (!strcmp(k, "fg"))          C_FG          = c;
            else if (!strcmp(k, "dim"))         C_DIM         = c;
            else if (!strcmp(k, "border"))      C_BORDER      = c;
        }
    } else if (!strcmp(sec, "spawn")) {
        unquote(v);
        if      (!strcmp(k, "terminal")) snprintf(spawn_terminal, 256, "%s", v);
        else if (!strcmp(k, "launcher")) snprintf(spawn_launcher, 256, "%s", v);
    } else if (!strcmp(sec, "general2")) {
        unquote(v);
        if (!strcmp(k, "scratch_class")) snprintf(scratch_class, 64, "%s", v);
    } else if (!strcmp(sec, "rules")) {
        unquote(k); unquote(v);
        if (n_rules < 64) {
            snprintf(rules[n_rules].match, 64, "%s", k);
            snprintf(rules[n_rules].act, 128, "%s", v);
            n_rules++;
        }
    } else if (!strcmp(sec, "binds")) {
        unquote(k); unquote(v);
        if (n_binds < 128) {
            snprintf(binds[n_binds].key, 64, "%s", k);
            snprintf(binds[n_binds].act, 256, "%s", v);
            n_binds++;
        }
    }
}
static void cfg_load(void) {
    n_binds = 0; n_marks = 0; n_rules = 0;
    border_w = 1;
    card_threshold = 0.95;
    zoom_min = 0.15;
    zoom_max = 1.00;
    spring_pan_k = 60.0;
    spring_zoom_k = 60.0;
    C_BG          = 0x0d1219;
    C_CARD        = 0x1a2436;
    C_CARD_BORDER = 0x2e3d54;
    C_FOCUS       = 0xe0a458;
    C_FG          = 0xe8e3d5;
    C_DIM         = 0x7a8595;
    C_BORDER      = 0x2a3548;
    snprintf(spawn_terminal, 256, "xterm -fa Monospace -fs 12");
    snprintf(spawn_launcher, 256, "dmenu_run");

    FILE *f = fopen(cfg_path, "r");
    if (!f) {
        char dir[512]; snprintf(dir, sizeof dir, "%s", cfg_path);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; mkdir(dir, 0755); }
        f = fopen(cfg_path, "w");
        if (f) { fputs(DEFAULT_CFG, f); fclose(f); }
        f = fopen(cfg_path, "r");
        if (!f) return;
    }
    char section[64] = "";
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (e) { *e = 0; snprintf(section, sizeof section, "%s", trim(p + 1)); }
            continue;
        }
        if (!strcmp(section, "marks")) { cfg_mark(p); continue; }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(p);
        char *v = trim(eq + 1);
        int in_q = 0;
        for (char *q = v; *q; q++) {
            if (*q == '"') in_q = !in_q;
            else if (*q == '#' && !in_q) { *q = 0; v = trim(v); break; }
        }
        cfg_kv(section, k, v);
    }
    fclose(f);
}

static void canon_key(const char *in, char *out, size_t outsz) {
    char buf[256]; snprintf(buf, sizeof buf, "%s", in);
    char *parts[16]; int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok && n < 16;
         tok = strtok_r(NULL, "+", &save)) {
        tok = trim(tok);
        if (*tok) {
            for (char *q = tok; *q; q++) *q = (char)toupper((unsigned char)*q);
            parts[n++] = tok;
        }
    }
    char *mods[16]; int nm = 0; char *key = NULL;
    for (int i = 0; i < n; i++) {
        if (!strcmp(parts[i], "ALT") || !strcmp(parts[i], "CTRL") ||
            !strcmp(parts[i], "SHIFT") || !strcmp(parts[i], "SUPER"))
            mods[nm++] = parts[i];
        else key = parts[i];
    }
    const char *order[] = {"ALT", "CTRL", "SHIFT", "SUPER", NULL};
    char *sorted[16]; int ns = 0;
    for (int o = 0; order[o]; o++)
        for (int i = 0; i < nm; i++)
            if (!strcmp(mods[i], order[o])) sorted[ns++] = mods[i];
    out[0] = 0;
    for (int i = 0; i < ns; i++) {
        strncat(out, sorted[i], outsz - strlen(out) - 1);
        strncat(out, "+",      outsz - strlen(out) - 1);
    }
    if (key) strncat(out, key, outsz - strlen(out) - 1);
}
static const char *bind_action(const char *key) {
    char norm[64];
    canon_key(key, norm, sizeof norm);
    for (int i = 0; i < n_binds; i++) {
        char bk[64];
        canon_key(binds[i].key, bk, sizeof bk);
        if (!strcmp(bk, norm)) return binds[i].act;
    }
    return NULL;
}

static void do_action(const char *action);
static void reload_config(void);
static void grab_all_keys(void);

static void handle_key(XKeyEvent *ev) {
    if (in_card_mode) {
        KeySym ks = XLookupKeysym(ev, 0);
        unsigned int st = ev->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
        if (!(st & Mod1Mask)) {
            if (ks == XK_Escape) {
                if (search_len > 0) { search_buf[0] = 0; search_len = 0; apply_view(); }
                else {
                    double tx = focused ? focused->px : view_cx;
                    double ty = focused ? focused->py : view_cy;
                    enter_real_mode();
                    anim_to(tx, ty, 1.0);
                }
                return;
            }
            if (ks == XK_Return || ks == XK_KP_Enter) {
                Client *mm = first_match();
                if (mm) {
                    search_buf[0] = 0; search_len = 0;
                    set_focus(mm);
                    enter_real_mode();
                    anim_to(mm->px, mm->py, 1.0);
                }
                return;
            }
            if (ks == XK_BackSpace) {
                if (search_len > 0) { search_len--; search_buf[search_len] = 0; apply_view(); }
                return;
            }
            char kb[8];
            int kn = XLookupString(ev, kb, sizeof kb, NULL, NULL);
            if (kn == 1 && (unsigned char)kb[0] >= 32 && (unsigned char)kb[0] < 127
                && search_len < 63) {
                search_buf[search_len++] = kb[0];
                search_buf[search_len] = 0;
                apply_view();
            }
            return;
        }
    }

    KeySym ks = XLookupKeysym(ev, 0);
    unsigned int st = ev->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
    char spec[64] = "";
    if (st & Mod1Mask)    strncat(spec, "Alt+",   sizeof spec - strlen(spec) - 1);
    if (st & ControlMask) strncat(spec, "Ctrl+",  sizeof spec - strlen(spec) - 1);
    if (st & ShiftMask)   strncat(spec, "Shift+", sizeof spec - strlen(spec) - 1);
    if (st & Mod4Mask)    strncat(spec, "Super+", sizeof spec - strlen(spec) - 1);
    const char *kn = XKeysymToString(ks);
    if (!kn) return;
    char keyup[64]; snprintf(keyup, sizeof keyup, "%s", kn);
    if (strlen(keyup) == 1 && keyup[0] >= 'a' && keyup[0] <= 'z')
        keyup[0] = (char)(keyup[0] - 32);
    strncat(spec, keyup, sizeof spec - strlen(spec) - 1);

    const char *act = bind_action(spec);
    if (act) do_action(act);
}

static void scratchpad_toggle(void) {
    if (in_card_mode) return;
    if (!scratch_c) {
        /* no scratch yet — spawn one and remember to claim it */
        scratch_pending = 1;
        spawn(spawn_terminal);
        return;
    }
    /* if it died since we last saw it, forget it */
    if (!find(scratch_c->win)) {
        scratch_c = NULL; scratch_hidden = 0;
        scratch_pending = 1;
        spawn(spawn_terminal);
        return;
    }
    if (scratch_hidden) {
        scratch_hidden = 0;
        scratch_c->hidden_by_wm = 0;
        XMapWindow(dpy, scratch_c->win);
        /* drop it near the camera, not at its old spot */
        scratch_c->px = view_cx;
        scratch_c->py = view_cy;
        save_position(scratch_c);
        set_focus(scratch_c);
        XRaiseWindow(dpy, scratch_c->win);
        apply_view();
    } else {
        scratch_hidden = 1;
        scratch_c->hidden_by_wm = 1;
        XUnmapWindow(dpy, scratch_c->win);
        if (focused == scratch_c) {
            focused = NULL;
            for (Client *c = clients; c; c = c->next) {
                if (c != scratch_c && !c->hidden_by_wm) { set_focus(c); break; }
            }
        }
        apply_view();
    }
}

static void session_save(void) {
    if (!session_path[0]) return;
    FILE *f = fopen(session_path, "w");
    if (!f) return;
    fprintf(f, "# atlas session\n");
    fprintf(f, "cam %.2f %.2f %.4f\n", view_cx, view_cy, view_zoom);
    fprintf(f, "scratch %s\n", scratch_class);
    fclose(f);
}
static void session_load(void) {
    if (!session_path[0]) return;
    FILE *f = fopen(session_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || !*p) continue;
        double a, b, c;
        if (sscanf(p, "cam %lf %lf %lf", &a, &b, &c) == 3) {
            view_cx = a; view_cy = b; view_zoom = c;
            tgt_cx = a; tgt_cy = b; tgt_zoom = c;
            if (view_zoom < zoom_min) view_zoom = zoom_min;
            if (view_zoom > zoom_max) view_zoom = zoom_max;
        } else if (sscanf(p, "scratch %63s", scratch_class) == 1) {
            /* keep whatever was in the file */
        }
    }
    fclose(f);
}

static void do_action(const char *action) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", action);
    char *save = NULL;
    char *verb = strtok_r(buf, " \t", &save);
    char *arg  = save;
    if (arg) { while (*arg == ' ' || *arg == '\t') arg++; if (!*arg) arg = NULL; }
    if (!verb) return;

    double step = 80.0 / view_zoom;

    if (!strcmp(verb, "spawn")) {
        if      (arg && !strcmp(arg, "terminal")) spawn(spawn_terminal);
        else if (arg && !strcmp(arg, "launcher")) spawn(spawn_launcher);
        else if (arg) spawn(arg);
    }
    else if (!strcmp(verb, "scratchpad")) scratchpad_toggle();
    else if (!strcmp(verb, "close"))  close_client(focused);
    else if (!strcmp(verb, "quit"))   running = 0;
    else if (!strcmp(verb, "reload")) reload_config();
    else if (!strcmp(verb, "card") && arg && !strcmp(arg, "toggle")) {
        if (in_card_mode) {
            double tx = focused ? focused->px : view_cx;
            double ty = focused ? focused->py : view_cy;
            enter_real_mode();
            anim_to(tx, ty, 1.0);
        } else fit_all();
    }
    else if (!strcmp(verb, "pan") && arg) {
        if      (!strcmp(arg, "left"))  pan(-step, 0);
        else if (!strcmp(arg, "right")) pan(+step, 0);
        else if (!strcmp(arg, "up"))    pan(0, -step);
        else if (!strcmp(arg, "down"))  pan(0, +step);
    }
    else if (!strcmp(verb, "move") && arg && focused) {
        if      (!strcmp(arg, "left"))  focused->px -= step;
        else if (!strcmp(arg, "right")) focused->px += step;
        else if (!strcmp(arg, "up"))    focused->py -= step;
        else if (!strcmp(arg, "down"))  focused->py += step;
        save_position(focused);
        apply_view();
    }
    else if (!strcmp(verb, "focus") && arg) {
        if      (!strcmp(arg, "left"))  focus_dir(0);
        else if (!strcmp(arg, "right")) focus_dir(1);
        else if (!strcmp(arg, "up"))    focus_dir(2);
        else if (!strcmp(arg, "down"))  focus_dir(3);
    }
    else if (!strcmp(verb, "zoom") && arg) {
        if      (!strcmp(arg, "in"))  zoom_at(view_zoom * 1.15, SW/2, SH/2);
        else if (!strcmp(arg, "out")) zoom_at(view_zoom / 1.15, SW/2, SH/2);
        else                          zoom_at(atof(arg), SW/2, SH/2);
    }
    else if (!strcmp(verb, "reset"))  reset_view();
    else if (!strcmp(verb, "fit") && arg && !strcmp(arg, "focused")) reveal(focused);
    else if (!strcmp(verb, "cycle") && arg) {
        if      (!strcmp(arg, "next")) cycle_focus(+1);
        else if (!strcmp(arg, "prev")) cycle_focus(-1);
    }
    else if (!strcmp(verb, "mark") && arg) {
        if      (!strcmp(arg, "save")) mark_save();
        else if (!strcmp(arg, "goto")) {
            char *a2 = strtok_r(NULL, " \t", &save);
            if (a2) mark_goto(a2);
        }
    }
    else if (!strcmp(verb, "goto") && arg) {
        char *a2 = strtok_r(NULL, " \t", &save);
        if (a2) anim_to(atof(arg), atof(a2), view_zoom);
    }
}

static void anim_tick(void) {
    if (!anim_active) return;
    const double dt = 0.016;
    const double K  = spring_pan_k;
    const double D  = 2.0 * 0.9 * sqrt(spring_pan_k);
    const double Kz = spring_zoom_k;
    const double Dz = 2.0 * 0.9 * sqrt(spring_zoom_k);

    double ax = -K * (view_cx - tgt_cx) - D * vel_cx;
    double ay = -K * (view_cy - tgt_cy) - D * vel_cy;
    double az = -Kz * ((view_zoom - tgt_zoom) * 800.0) - Dz * vel_zoom;

    vel_cx   += ax * dt; vel_cy += ay * dt; vel_zoom += az * dt;
    view_cx   += vel_cx * dt; view_cy += vel_cy * dt; view_zoom += vel_zoom * dt;

    double dx = tgt_cx - view_cx, dy = tgt_cy - view_cy, dz = tgt_zoom - view_zoom;
    double sp = fabs(vel_cx) + fabs(vel_cy) + fabs(vel_zoom) * 800.0;
    if (dx*dx + dy*dy + dz*dz*800.0*800.0 < 0.02 && sp < 1.5) {
        view_cx = tgt_cx; view_cy = tgt_cy; view_zoom = tgt_zoom;
        vel_cx = vel_cy = vel_zoom = 0;
        anim_active = 0;
    }
    apply_view();
}

static void ipc_send(int fd, const char *fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof b - 1) n = sizeof b - 1;
        ssize_t w = write(fd, b, (size_t)n); (void)w;
    }
}
static void ipc_cmd(int fd, const char *line) {
    char buf[1024]; snprintf(buf, sizeof buf, "%s", line);
    char *sp = NULL;
    char *cmd = strtok_r(buf, " \t\n", &sp);
    if (!cmd) { ipc_send(fd, "err empty\n"); return; }

    if (!strcmp(cmd, "status")) {
        int n = 0;
        for (Client *c = clients; c; c = c->next) n++;
        ipc_send(fd, "zoom %.3f\n", view_zoom);
        ipc_send(fd, "cam %.1f %.1f\n", view_cx, view_cy);
        ipc_send(fd, "card %d\n", in_card_mode);
        ipc_send(fd, "count %d\n", n);
        if (focused && focused->cls[0])
            ipc_send(fd, "focused %s\n", focused->cls);
        else if (focused && focused->title[0])
            ipc_send(fd, "focused %.60s\n", focused->title);
        else
            ipc_send(fd, "focused none\n");
    }
    else if (!strcmp(cmd, "list")) {
        int i = 0;
        for (Client *c = clients; c; c = c->next, i++)
            ipc_send(fd, "%d\t%s\t%s\t%.1f\t%.1f\t%d\t%d\t%s\n",
                     i, c->cls[0] ? c->cls : "?",
                     c->title[0] ? c->title : "?",
                     c->px, c->py, c->w, c->h,
                     c->rule[0] ? c->rule : "-");
    }
    else if (!strcmp(cmd, "goto")) {
        char *a = strtok_r(NULL, " \t", &sp);
        char *b = strtok_r(NULL, " \t", &sp);
        if (a && b) { anim_to(atof(a), atof(b), view_zoom); ipc_send(fd, "ok\n"); }
        else ipc_send(fd, "err usage\n");
    }
    else if (!strcmp(cmd, "zoom")) {
        char *a = strtok_r(NULL, " \t", &sp);
        if (a) { zoom_at(atof(a), SW/2, SH/2); ipc_send(fd, "ok\n"); }
        else   ipc_send(fd, "err usage\n");
    }
    else if (!strcmp(cmd, "locate")) {
        char *pat = strtok_r(NULL, " \t", &sp);
        if (!pat) { ipc_send(fd, "err usage\n"); return; }
        for (Client *c = clients; c; c = c->next) {
            if (strcasestr(c->cls, pat) || strcasestr(c->title, pat)) {
                set_focus(c);
                anim_to(c->px, c->py, 1.0);
                ipc_send(fd, "ok %s\n", c->cls[0] ? c->cls : c->title);
                return;
            }
        }
        ipc_send(fd, "err no match\n");
    }
    else if (!strcmp(cmd, "focus")) {
        char *pat = strtok_r(NULL, " \t", &sp);
        if (!pat) { ipc_send(fd, "err usage\n"); return; }
        for (Client *c = clients; c; c = c->next) {
            if (strcasestr(c->cls, pat) || strcasestr(c->title, pat)) {
                set_focus(c); ipc_send(fd, "ok\n"); return;
            }
        }
        ipc_send(fd, "err no match\n");
    }
    else if (!strcmp(cmd, "spawn")) {
        while (sp && (*sp == ' ' || *sp == '\t')) sp++;
        if (sp && *sp) { spawn(sp); ipc_send(fd, "ok\n"); }
        else ipc_send(fd, "err usage\n");
    }
    else if (!strcmp(cmd, "card")) {
        if (in_card_mode) { enter_real_mode(); anim_to(view_cx, view_cy, 1.0); }
        else fit_all();
        ipc_send(fd, "ok\n");
    }
    else if (!strcmp(cmd, "mark")) {
        char *sub = strtok_r(NULL, " \t", &sp);
        if (sub && !strcmp(sub, "save")) {
            char *name = strtok_r(NULL, " \t", &sp);
            if (name) {
                /* save under a custom name (overwrite if exists) */
                int idx = -1;
                for (int i = 0; i < n_marks; i++)
                    if (!strcmp(marks[i].name, name)) { idx = i; break; }
                if (idx < 0) { if (n_marks >= 32) { ipc_send(fd, "err full\n"); return; }
                               idx = n_marks++; }
                snprintf(marks[idx].name, 32, "%s", name);
                marks[idx].x = view_cx; marks[idx].y = view_cy;
                marks_save();
                ipc_send(fd, "ok\n");
            } else {
                mark_save();
                ipc_send(fd, "ok\n");
            }
        } else if (sub && !strcmp(sub, "goto")) {
            char *name = strtok_r(NULL, " \t", &sp);
            if (name && mark_goto(name)) ipc_send(fd, "ok\n");
            else                         ipc_send(fd, "err no such mark\n");
        } else {
            for (int i = 0; i < n_marks; i++)
                ipc_send(fd, "%s\t%.1f\t%.1f\n",
                         marks[i].name, marks[i].x, marks[i].y);
        }
    }
    else if (!strcmp(cmd, "marks")) {
        for (int i = 0; i < n_marks; i++)
            ipc_send(fd, "%s\t%.1f\t%.1f\n", marks[i].name, marks[i].x, marks[i].y);
    }
    else if (!strcmp(cmd, "rules")) {
        for (int i = 0; i < n_rules; i++)
            ipc_send(fd, "%s\t%s\n", rules[i].match, rules[i].act);
    }
    else if (!strcmp(cmd, "scratchpad")) {
        scratchpad_toggle();
        ipc_send(fd, "ok\n");
    }
    else if (!strcmp(cmd, "focus-idx")) {
        char *a = strtok_r(NULL, " \t", &sp);
        if (!a) { ipc_send(fd, "err usage\n"); return; }
        int want = atoi(a), i = 0;
        for (Client *c = clients; c; c = c->next, i++) {
            if (i == want) {
                set_focus(c);
                anim_to(c->px, c->py, view_zoom);
                ipc_send(fd, "ok\n");
                return;
            }
        }
        ipc_send(fd, "err no such window\n");
    }
    else if (!strcmp(cmd, "close-idx")) {
        char *a = strtok_r(NULL, " \t", &sp);
        if (!a) { ipc_send(fd, "err usage\n"); return; }
        int want = atoi(a), i = 0;
        for (Client *c = clients; c; c = c->next, i++) {
            if (i == want) { close_client(c); ipc_send(fd, "ok\n"); return; }
        }
        ipc_send(fd, "err no such window\n");
    }
    else if (!strcmp(cmd, "quit")) {
        ipc_send(fd, "ok\n");
        XSync(dpy, False);
        running = 0;
    }
    else ipc_send(fd, "err unknown '%s'\n", cmd);
}
static void ipc_handle(void) {
    int c = accept(ipc_fd, NULL, NULL);
    if (c < 0) return;
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[2048];
    ssize_t n = read(c, buf, sizeof buf - 1);
    if (n > 0) { buf[n] = 0; ipc_cmd(c, buf); }
    close(c);
}
static void ipc_init(void) {
    const char *path = "/tmp/atlas.sock";
    unlink(path);
    ipc_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ipc_fd < 0) return;
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof a.sun_path - 1);
    if (bind(ipc_fd, (struct sockaddr *)&a, sizeof a) < 0) { close(ipc_fd); ipc_fd = -1; return; }
    chmod(path, 0600);
    if (listen(ipc_fd, 8) < 0) { close(ipc_fd); ipc_fd = -1; return; }
    fprintf(stderr, "atlas: ipc %s\n", path);
}

static void grab_key(KeySym ks, unsigned int mod) {
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (!kc) return;
    XGrabKey(dpy, kc, mod, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, kc, mod | LockMask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, kc, mod | Mod2Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, kc, mod | LockMask | Mod2Mask, root, True, GrabModeAsync, GrabModeAsync);
}
static void grab_all_keys(void) {
    for (int i = 0; i < n_binds; i++) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s", binds[i].key);
        unsigned int mods = 0;
        char keyname[64] = "";
        char *save = NULL;
        for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
            while (*tok == ' ') tok++;
            if      (!strcasecmp(tok, "Alt"))   mods |= Mod1Mask;
            else if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control")) mods |= ControlMask;
            else if (!strcasecmp(tok, "Shift")) mods |= ShiftMask;
            else if (!strcasecmp(tok, "Super")) mods |= Mod4Mask;
            else snprintf(keyname, sizeof keyname, "%s", tok);
        }
        if (!keyname[0]) continue;
        KeySym ks = XStringToKeysym(keyname);
        if (ks == NoSymbol) {
            for (char *p = keyname; *p; p++) *p = (char)tolower((unsigned char)*p);
            ks = XStringToKeysym(keyname);
        }
        if (ks == NoSymbol) continue;
        grab_key(ks, mods);
    }
}
static void grab_buttons(void) {
    XGrabButton(dpy, Button1, 0, root, False,
                ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button1, Mod4Mask, root, False,
                ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, Mod4Mask, root, False,
                ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button2, Mod4Mask, root, False,
                ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
}

static void reload_config(void) {
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    cfg_load();
    marks_load();
    grab_all_keys();
    apply_view();
    fprintf(stderr, "atlas: reloaded (%d binds, %d marks)\n", n_binds, n_marks);
}

static int x_error_handler(Display *d, XErrorEvent *e) {
    (void)d;
    switch (e->error_code) {
    case BadWindow: case BadDrawable: case BadMatch:
    case BadValue:  case BadAccess:   case BadAlloc:
    case BadPixmap: case BadCursor:   case BadGC:
        return 0;
    default: {
        char buf[256];
        XGetErrorText(d, e->error_code, buf, sizeof buf);
        fprintf(stderr, "atlas: X error: %s (req %d)\n", buf, e->request_code);
        return 0;
    }
    }
}
static int x_io_handler(Display *d) {
    (void)d;
    fprintf(stderr, "atlas: X connection lost\n");
    running = 0;
    return 0;
}

static void detect_primary(void) {
    int ev, er;
    if (!XRRQueryExtension(dpy, &ev, &er)) goto fallback;
    {
        Window rr = RootWindow(dpy, scr);
        int cnt = 0;
        XRRMonitorInfo *mons = XRRGetMonitors(dpy, rr, True, &cnt);
        if (mons && cnt > 0) {
            int idx = -1;

            /* 1. try config override by name */
            if (cfg_monitor[0] && XRRGetMonitors) {
                for (int i = 0; i < cnt; i++) {
                    if (mons[i].name && !strcmp(XGetAtomName(dpy, mons[i].name), cfg_monitor)) {
                        idx = i; break;
                    }
                }
            }
            /* 2. try config override by index */
            if (idx < 0 && cfg_monitor[0]) {
                char *end;
                long v = strtol(cfg_monitor, &end, 10);
                if (*end == 0 && v >= 0 && v < cnt) idx = v;
            }
            /* 3. X's primary flag */
            if (idx < 0) {
                for (int i = 0; i < cnt; i++) if (mons[i].primary) { idx = i; break; }
            }
            /* 4. fall back to first */
            if (idx < 0) idx = 0;

            mon_x = mons[idx].x;
            mon_y = mons[idx].y;
            SW = mons[idx].width;
            SH = mons[idx].height;

            char *name = mons[idx].name ? XGetAtomName(dpy, mons[idx].name) : (char*)"?";
            fprintf(stderr, "atlas: monitor[%d] '%s' %dx%d+%d+%d%s\n",
                    idx, name, SW, SH, mon_x, mon_y,
                    mons[idx].primary ? " [primary]" : "");
            if (mons[idx].name) XFree(name);

            XRRFreeMonitors(mons);
            return;
        }
        if (mons) XRRFreeMonitors(mons);
    }
fallback:
    mon_x = 0;
    mon_y = 0;
    SW = DisplayWidth(dpy, scr);
    SH = DisplayHeight(dpy, scr);
    fprintf(stderr, "atlas: single screen %dx%d\n", SW, SH);
}


int main(int argc, char **argv) {
    int check_only = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--check")) check_only = 1;

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    XSetErrorHandler(x_error_handler);
    XSetIOErrorHandler(x_io_handler);

    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(cfg_path,  sizeof cfg_path,  "%s/.config/atlas/config", home);
    snprintf(mark_path, sizeof mark_path, "%s/.config/atlas/marks",  home);
    {
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && *xdg) snprintf(session_path, sizeof session_path, "%s/atlas/session", xdg);
        else snprintf(session_path, sizeof session_path, "%s/.cache/atlas/session", home);
    }

    cfg_load();
    marks_load();
    session_load();

    if (check_only) {
        printf("config: %s\n", cfg_path);
        printf("  %d binds, %d marks, %d rules\n", n_binds, n_marks, n_rules);
        printf("  card_threshold=%.0f%% zoom=[%.0f..%.0f]%%\n",
               card_threshold*100, zoom_min*100, zoom_max*100);
        printf("  spawn: %s\n", spawn_terminal);
        return 0;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "atlas: no display\n"); return 1; }
    scr  = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    detect_primary();

    A_WMNAME  = XInternAtom(dpy, "_NET_WM_NAME", False);
    A_UTF8    = XInternAtom(dpy, "UTF8_STRING", False);
    A_ATLAS_X = XInternAtom(dpy, "_ATLAS_X", False);
    A_ATLAS_Y = XInternAtom(dpy, "_ATLAS_Y", False);

    gc = XCreateGC(dpy, root, 0, NULL);

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof attrs);
    attrs.override_redirect = True;
    attrs.background_pixel  = alloc_color(C_BG);
    attrs.event_mask        = 0;
    card_win = XCreateWindow(dpy, root, mon_x, mon_y, SW, SH, 0,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);

    XSelectInput(dpy, root,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                 KeyPressMask | ButtonPressMask | PointerMotionMask |
                 EnterWindowMask);

    XSync(dpy, False);
    grab_all_keys();
    grab_buttons();

    Window r, p, *kids; unsigned int n;
    if (XQueryTree(dpy, root, &r, &p, &kids, &n)) {
        for (unsigned int i = 0; i < n; i++) {
            XWindowAttributes wa;
            if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
            if (wa.override_redirect) continue;
            if (wa.map_state != IsViewable) continue;
            manage(kids[i], &wa);
        }
        if (kids) XFree(kids);
    }

    ipc_init();

    fprintf(stderr, "atlas: started (%dx%d, %d binds, %d marks)\n",
            SW, SH, n_binds, n_marks);

    int xfd = ConnectionNumber(dpy);
    while (running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case MapRequest: {
                XWindowAttributes wa;
                if (!XGetWindowAttributes(dpy, ev.xmaprequest.window, &wa)) break;
                if (wa.override_redirect) { XMapWindow(dpy, ev.xmaprequest.window); break; }
                if (find(ev.xmaprequest.window)) break;
                manage(ev.xmaprequest.window, &wa);
                break;
            }
            case ConfigureRequest: {
                XConfigureRequestEvent *cr = &ev.xconfigurerequest;
                XWindowChanges wc; memset(&wc, 0, sizeof wc);
                wc.x = cr->x; wc.y = cr->y;
                wc.width = cr->width; wc.height = cr->height;
                wc.border_width = cr->border_width;
                wc.sibling = cr->above; wc.stack_mode = cr->detail;
                XConfigureWindow(dpy, cr->window, cr->value_mask, &wc);
                Client *c = find(cr->window);
                if (c) {
                    if (cr->value_mask & CWWidth)  c->w = cr->width;
                    if (cr->value_mask & CWHeight) c->h = cr->height;
                    apply_view();
                }
                break;
            }
            case KeyPress:
                handle_key(&ev.xkey);
                break;
            case ButtonPress: {
                unsigned int st = ev.xbutton.state &
                    (ShiftMask|ControlMask|Mod1Mask|Mod4Mask);
                if (ev.xbutton.button == Button4) {
                    zoom_at(view_zoom * 1.15, ev.xbutton.x_root, ev.xbutton.y_root);
                    break;
                }
                if (ev.xbutton.button == Button5) {
                    zoom_at(view_zoom / 1.15, ev.xbutton.x_root, ev.xbutton.y_root);
                    break;
                }
                if (ev.xbutton.button == Button2 && (st & Mod4Mask)) {
                    int lx = ev.xbutton.x_root, ly = ev.xbutton.y_root;
                    XGrabPointer(dpy, root, False,
                                 PointerMotionMask | ButtonReleaseMask,
                                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                    for (;;) {
                        XEvent e2; XNextEvent(dpy, &e2);
                        if (e2.type == MotionNotify) {
                            double dx = (e2.xmotion.x_root - lx) / view_zoom;
                            double dy = (e2.xmotion.y_root - ly) / view_zoom;
                            view_cx -= dx; view_cy -= dy;
                            lx = e2.xmotion.x_root; ly = e2.xmotion.y_root;
                            anim_kill(); apply_view();
                        } else if (e2.type == ButtonRelease) break;
                    }
                    XUngrabPointer(dpy, CurrentTime);
                    break;
                }
                if (ev.xbutton.button == Button3 && (st & Mod4Mask) && !in_card_mode) {
                    Client *c = client_at(ev.xbutton.x_root, ev.xbutton.y_root);
                    if (c) start_resize(c, ev.xbutton.x_root, ev.xbutton.y_root);
                    break;
                }
                if (ev.xbutton.button == Button1 && in_card_mode) {
                    int start_x = ev.xbutton.x_root, start_y = ev.xbutton.y_root;
                    int lx = start_x, ly = start_y, moved = 0;
                    XGrabPointer(dpy, root, False,
                                 PointerMotionMask | ButtonReleaseMask,
                                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
                    for (;;) {
                        XEvent e2; XNextEvent(dpy, &e2);
                        if (e2.type == MotionNotify) {
                            double dx = (e2.xmotion.x_root - lx) / view_zoom;
                            double dy = (e2.xmotion.y_root - ly) / view_zoom;
                            view_cx -= dx; view_cy -= dy;
                            lx = e2.xmotion.x_root; ly = e2.xmotion.y_root;
                            anim_kill(); apply_view();
                            if (abs(e2.xmotion.x_root - start_x) > 4 ||
                                abs(e2.xmotion.y_root - start_y) > 4) moved = 1;
                        } else if (e2.type == ButtonRelease) break;
                    }
                    XUngrabPointer(dpy, CurrentTime);
                    if (!moved) {
                        Client *c = client_at(start_x, start_y);
                        if (c) set_focus(c);
                    }
                    break;
                }
                if (ev.xbutton.button == Button1) {
                    Client *c = client_at(ev.xbutton.x_root, ev.xbutton.y_root);
                    if (st & Mod4Mask) {
                        if (c) {
                            start_drag(c, ev.xbutton.x_root, ev.xbutton.y_root);
                        } else {
                            /* Super+LMB on empty space — pan the map */
                            int lx = ev.xbutton.x_root, ly = ev.xbutton.y_root;
                            XGrabPointer(dpy, root, False,
                                         PointerMotionMask | ButtonReleaseMask,
                                         GrabModeAsync, GrabModeAsync,
                                         None, None, CurrentTime);
                            for (;;) {
                                XEvent e2; XNextEvent(dpy, &e2);
                                if (e2.type == MotionNotify) {
                                    double dx = (e2.xmotion.x_root - lx) / view_zoom;
                                    double dy = (e2.xmotion.y_root - ly) / view_zoom;
                                    view_cx -= dx; view_cy -= dy;
                                    lx = e2.xmotion.x_root;
                                    ly = e2.xmotion.y_root;
                                    anim_kill();
                                    apply_view();
                                } else if (e2.type == ButtonRelease) break;
                            }
                            XUngrabPointer(dpy, CurrentTime);
                        }
                    } else {
                        XAllowEvents(dpy, ReplayPointer, ev.xbutton.time);
                    }
                    break;
                }
                break;
            }
            case MotionNotify:
                if (drag_c) {
                    double dx = (ev.xmotion.x_root - drag_sx) / view_zoom;
                    double dy = (ev.xmotion.y_root - drag_sy) / view_zoom;
                    if (drag_mode == 1) {
                        drag_c->px = drag_opx + dx;
                        drag_c->py = drag_opy + dy;
                    } else {
                        int nw = drag_ow + (int)dx;
                        int nh = drag_oh + (int)dy;
                        if (nw < 80) nw = 80;
                        if (nh < 60) nh = 60;
                        drag_c->px = drag_opx + (nw - drag_ow) / 2.0;
                        drag_c->py = drag_opy + (nh - drag_oh) / 2.0;
                        drag_c->w = nw; drag_c->h = nh;
                    }
                    apply_view();
                }
                break;
            case ButtonRelease:
                if (drag_c) {
                    save_position(drag_c);
                    drag_c = NULL; drag_mode = 0;
                    XUngrabPointer(dpy, CurrentTime);
                }
                break;
            case EnterNotify: {
                if (in_card_mode) break;
                if (ev.xcrossing.mode != NotifyNormal) break;
                if (ev.xcrossing.detail == NotifyInferior) break;
                if (drag_c) break;
                Window w = ev.xcrossing.window;
                if (w == root || w == None) w = ev.xcrossing.subwindow;
                if (w == root || w == None) break;
                Client *c = find(w);
                if (!c) c = find(ev.xcrossing.subwindow);
                if (c && c != focused) {
                    focused = c;
                    if (!in_card_mode)
                        XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
                    apply_view();
                }
                break;
            }
            case DestroyNotify: {
                Client *c = find(ev.xdestroywindow.window);
                if (c) unmanage(c);
                break;
            }
            case UnmapNotify: {
                Client *c = find(ev.xunmap.window);
                if (c) {
                    if (c->hidden_by_wm) break;
                    unmanage(c);
                }
                break;
            }
            case PropertyNotify: {
                Client *c = find(ev.xproperty.window);
                if (c && (ev.xproperty.atom == A_WMNAME ||
                          ev.xproperty.atom == XA_WM_NAME))
                    update_title(c);
                if (in_card_mode) apply_view();
                break;
            }
            }
        }
        anim_tick();
        fd_set fds; FD_ZERO(&fds); FD_SET(xfd, &fds);
        int mx = xfd;
        if (ipc_fd >= 0) { FD_SET(ipc_fd, &fds); if (ipc_fd > mx) mx = ipc_fd; }
        struct timeval tv = {0, 16000};
        if (select(mx + 1, &fds, NULL, NULL, &tv) > 0) {
            if (ipc_fd >= 0 && FD_ISSET(ipc_fd, &fds)) ipc_handle();
        }
    }

    for (Client *c = clients; c; c = c->next) save_position(c);
    session_save();
    if (ipc_fd >= 0) { close(ipc_fd); unlink("/tmp/atlas.sock"); }
    XCloseDisplay(dpy);
    return 0;
}
