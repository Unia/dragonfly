/* see license for copyright and license */

#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/Xproto.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>

#define LENGTH(x)       (sizeof(x)/sizeof(*x))
#define MAX(A, B)       ((A) > (B) ? (A) : (B))
#define MIN(A, B)       ((A) < (B) ? (A) : (B))
#define WIDTH(X)        ((X)->w + 2*(X)->bw)
#define HEIGHT(X)       ((X)->h + 2*(X)->bw)
#define CLEANMASK(mask) (mask & ~(numlockmask | LockMask))
#define BUTTONMASK      ButtonPressMask|ButtonReleaseMask
#define ISFFT(c)        (c->isfull || c->isfloat || c->istrans)
#define ROOTMASK        SubstructureRedirectMask|ButtonPressMask|SubstructureNotifyMask|PropertyChangeMask

enum { RESIZE, MOVE };
enum { CLIENTWIN, ROOTWIN };
enum { TILE, MONOCLE, BSTACK, GRID, FLOAT, MODES };
enum { WM_PROTOCOLS, WM_DELETE_WINDOW, WM_STATE, WM_COUNT };
enum { NET_ACTIVE_WINDOW, NET_CLOSE_WINDOW, NET_SUPPORTED,
       NET_SUPPORTING_WM_CHECK, NET_WM_NAME, NET_CLIENT_LIST,
       NET_CLIENT_LIST_STACKING, NET_NUMBER_OF_DESKTOPS,
       NET_CURRENT_DESKTOP, NET_DESKTOP_NAMES, NET_WM_DESKTOP,
       NET_WM_STATE, NET_WM_STATE_ABOVE, NET_WM_STATE_FULLSCREEN,
       NET_WM_STATE_DEMANDS_ATTENTION, NET_WM_WINDOW_TYPE,
       NET_WM_WINDOW_TYPE_DOCK, NET_WM_WINDOW_TYPE_DESKTOP,
       NET_WM_WINDOW_TYPE_SPLASH, NET_WM_WINDOW_TYPE_MENU,
       NET_WM_WINDOW_TYPE_DIALOG, NET_WM_WINDOW_TYPE_UTILITY,
       UTF8_STRING, NET_COUNT };

/**
 * argument structure to be passed to function by config.h
 * com - function pointer ~ the command to run
 * i   - an integer to indicate different states
 * v   - any type argument
 */
typedef union {
    const char** com;
    const int i;
    const void *v;
} Arg;

/**
 * a key struct represents a combination of
 * mod    - a modifier mask
 * keysym - and the key pressed
 * func   - the function to be triggered because of the above combo
 * arg    - the argument to the function
 */
typedef struct {
    unsigned int mod;
    KeySym keysym;
    void (*func)(const Arg *);
    const Arg arg;
} Key;

/**
 * a button struct represents a combination of
 * mask   - a modifier mask
 * button - and the mouse button pressed
 * func   - the function to be triggered because of the above combo
 * arg    - the argument to the function
 */
typedef struct {
    unsigned int click, mask, button;
    void (*func)(const Arg *);
    const Arg arg;
} Button;

/**
 * define behavior of certain applications
 * configured in config.h
 *
 * class       - the class or name of the instance
 * instance    - the instance of the instance
 * title       - the window title of the instance
 * desktop     - what desktop it should be spawned at
 * follow      - whether to change desktop focus to the specified desktop
 * attachaside - whether this client attaches on last in the stack
 */
typedef struct {
    const char *class, *instance, *title;
    const int desktop;
    const Bool follow, floating, attachaside;
} AppRule;

typedef struct {
    const char *name;
    const int mode;
    float mfact;
    int nm;
    Bool sbar;
} DeskSettings;

/* exposed function prototypes sorted alphabetically */
static void change_desktop(const Arg *arg);
static void client_to_desktop(const Arg *arg);
static void focusurgent();
static void killclient();
static void last_desktop();
static void move_down();
static void move_up();
static void moveresize(const Arg *arg);
static void mousemotion(const Arg *arg);
static void next_win();
static void nmaster(const Arg *arg);
static void prev_win();
static void quit(const Arg *arg);
static void resize_master(const Arg *arg);
static void resize_stack(const Arg *arg);
static void rotate(const Arg *arg);
static void rotate_filled(const Arg *arg);
static void spawn(const Arg *arg);
static void swap_master();
static void switch_mode(const Arg *arg);
static void togglefloat();
static void togglepanel();

#include "config.h"

/**
 * a client is a wrapper to a window that additionally
 * holds some properties for that window
 *
 * next    - the client after this one, or NULL if the current is the last client
 * isurgn  - set when the window received an urgent hint
 * isfull  - set when the window is fullscreen
 * isfloat - set when the window is floating
 * istrans - set when the window is transient
 * isfixed - set when the window has a fixed size
 * win     - the window this client is representing
 *
 * istrans is separate from isfloat as floating windows can be reset to
 * their tiling positions, while the transients will always be floating
 */
typedef struct Client {
    struct Client *next;
    Bool isurgn, isfull, isfloat, istrans, isfixed;
    Window win;
    float mina, maxa;
    int x, y, w, h, bw;
    int oldx, oldy, oldw, oldh, oldbw;
    int basew, baseh, incw, inch, maxw, maxh, minw, minh;
} Client;

/**
 * properties of each desktop
 *
 * masz - the size of the master area
 * sasz - additional size of the first stack window area
 * mode - the desktop's tiling layout mode
 * nm   - the number of windows in the master area
 * head - the start of the client list
 * curr - the currently highlighted window
 * prev - the client that previously had focus
 * sbar - the visibility status of the panel/statusbar
 * name - the name of the desktop
 */
typedef struct {
    int mode, masz, sasz, nm;
    Client *head, *curr, *prev;
    float mfact;
    Bool sbar;
    const char *name;
} Desktop;

/* hidden function prototypes sorted alphabetically */
static Client* addwindow(Window w, Desktop *d, Bool attachaside);
static Bool applysizehints(Client *c, int *x, int *y, int *w, int *h, Bool interact);
static void buttonpress(XEvent *e);
static void cleanup(void);
static void clientmessage(XEvent *e);
static void configure(Client *c);
static void configurerequest(XEvent *e);
static void deletewindow(Window w);
static void destroynotify(XEvent *e);
static void enternotify(XEvent *e);
static void focus(Client *c, Desktop *d);
static void focusin(XEvent *e);
static unsigned long getcolor(const char* color, const int screen);
static void grabbuttons(Client *c);
static void grabkeys(void);
static void grid(int x, int y, int w, int h, const Desktop *d);
static void keypress(XEvent *e);
static void maprequest(XEvent *e);
static void monocle(int x, int y, int w, int h, const Desktop *d);
static Client* prevclient(Client *c, Desktop *d);
static void propertynotify(XEvent *e);
static void removeclient(Client *c, Desktop *d);
static void resize(Client *c, int x, int y, int w, int h, Bool interact);
static void resizeclient(Client *c, int x, int y, int w, int h);
static void run(void);
static void setclientstate(Client *c, long state);
static void setdesktopnames(void);
static void setfullscreen(Client *c, Desktop *d, Bool fullscreen);
static void setnumberofdesktops(void);
static void setup(void);
static void sigchld(int sig);
static void stack(int x, int y, int w, int h, const Desktop *d);
static void tile(Desktop *d);
static void unmapnotify(XEvent *e);
static void updateclientdesktop(Client *c, int desktop);
static void updateclientlist(void);
static void updatecurrentdesktop(void);
static void updatesizehints(Client *c);
static Bool wintoclient(Window w, Client **c, Desktop **d);
static int xerror(Display *dis, XErrorEvent *ee);
static int xerrorstart(Display *dis, XErrorEvent *ee);

/**
 * global variables
 *
 * running      - whether the wm is accepting and processing more events
 * wh           - screen height
 * ww           - screen width
 * dis          - the display aka dpy
 * root         - the root window
 * wmatoms      - array holding atoms for ICCCM support
 * netatoms     - array holding atoms for EWMH support
 * desktops     - array of managed desktops
 * currdeskidx  - which desktop is currently active
 */
static Bool running = True;
static int wh, ww, currdeskidx, prevdeskidx, retval;
static unsigned int numlockmask, win_unfocus, win_focus, cur_norm, cur_move, cur_res;
static const char wmname[12] = "DragonflyWM";
static Display *dis;
static Window root, supportwin;
static Atom wmatoms[WM_COUNT], netatoms[NET_COUNT];
static Desktop desktops[DESKTOPS];

/**
 * array of event handlers
 *
 * when a new event is received,
 * call the appropriate handler function
 */
static void (*events[LASTEvent])(XEvent *e) = {
    [KeyPress]         = keypress,     [EnterNotify]    = enternotify,
    [MapRequest]       = maprequest,   [ClientMessage]  = clientmessage,
    [ButtonPress]      = buttonpress,  [DestroyNotify]  = destroynotify,
    [UnmapNotify]      = unmapnotify,  [PropertyNotify] = propertynotify,
    [ConfigureRequest] = configurerequest,    [FocusIn] = focusin,
};

/**
 * array of layout handlers
 *
 * x - the start position in the x axis to place clients
 * y - the start position in the y axis to place clients
 * w - available width  that windows have to expand
 * h - available height that windows have to expand
 * d - the desktop to tile its clients
 */
static void (*layout[MODES])(int x, int y, int w, int h, const Desktop *d) = {
    [TILE] = stack, [BSTACK] = stack, [GRID] = grid, [MONOCLE] = monocle,
};

/**
 * add the given window to the given desktop
 *
 * create a new client to hold the new window
 *
 * if there is no head at the given desktop
 * add the window as the head
 * otherwise if attachaside is not set,
 * add the window as the last client
 * otherwise add the window as head
 */
Client* addwindow(Window w, Desktop *d, Bool attachaside) {
    Client *c = NULL, *t = prevclient(d->head, d);
    if (!(c = (Client *)calloc(1, sizeof(Client)))) err(EXIT_FAILURE, "%s: cannot allocate client", wmname);
    if (!d->head) d->head = c;
    else if (!attachaside) { c->next = d->head; d->head = c; }
    else if (t) t->next = c; else d->head->next = c;

    XSelectInput(dis, (c->win = w), PropertyChangeMask|FocusChangeMask|(follow_mouse?EnterWindowMask:0));
    return c;
}

Bool applysizehints(Client *c, int *x, int *y, int *w, int *h, Bool interact) {
    Desktop *d = &desktops[currdeskidx];
    Bool baseismin;

    /* set minimum possible */
    *w = MAX(1, *w);
    *h = MAX(1, *h);
    if (interact) {
        if (*x > ww) *x = ww - WIDTH(c);
        if (*y > wh) *y = wh - HEIGHT(c);
        if (*x + *w + 2*c->bw < 0) *x = 0;
        if (*y + *h + 2*c->bw < 0) *y = 0;
    } else {
        if (*x >= ww) *x = ww - WIDTH(c);
        if (*y >= wh) *y = wh - HEIGHT(c);
    }
    if (*h < minwsz) *h = minwsz;
    if (*w < minwsz) *w = minwsz;
    if (resizehints || c->isfloat || d->mode == FLOAT) {
        /* see last two sentences in ICCCM 4.1.2.3 */
        baseismin = c->basew == c->minw && c->baseh == c->minh;
        if (!baseismin) { /* temporarily remove base dimensions */
            *w -= c->basew;
            *h -= c->baseh;
        }
        /* adjust for aspect limits */
        if (c->mina > 0 && c->maxa > 0) {
            if (c->maxa < (float)*w / *h) *w = *h * c->maxa + 0.5;
            else if (c->mina < (float)*h / *w) *h = *w * c->mina + 0.5;
        }
        if (baseismin) { /* increment calculation requires this */
            *w -= c->basew;
            *h -= c->baseh;
        }
        /* adjust for increment value */
        if (c->incw) *w -= *w % c->incw;
        if (c->inch) *h -= *h % c->inch;
        /* restore base dimensions */
        *w = MAX(*w + c->basew, c->minw);
        *h = MAX(*h + c->baseh, c->minh);
        if (c->maxw) *w = MIN(*w, c->maxw);
        if (c->maxh) *h = MIN(*h, c->maxh);
    }
    return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

/**
 * on the press of a key binding (see grabkeys)
 * call the appropriate handler
 */
void buttonpress(XEvent *e) {
    Desktop *d = NULL; Client *c = NULL;
    unsigned int click = ROOTWIN;

    if (wintoclient(e->xbutton.window, &c, &d)) { focus(c, d); click = CLIENTWIN; }

    for (unsigned int i = 0; i < LENGTH(buttons); i++)
        if (click == buttons[i].click && CLEANMASK(buttons[i].mask) == CLEANMASK(e->xbutton.state) &&
            buttons[i].func && buttons[i].button == e->xbutton.button) {
            if (c && d->curr != c) focus(c, d);
            buttons[i].func(&(buttons[i].arg));
        }
}

/**
 * focus another desktop
 *
 * to avoid flickering (esp. monocle mode):
 * first map the new windows
 * first the current window and then all other
 * then unmap the old windows
 * first all others then the current
 */
void change_desktop(const Arg *arg) {
    if (arg->i == currdeskidx || arg->i < 0 || arg->i >= DESKTOPS) return;
    Desktop *d = &desktops[(prevdeskidx = currdeskidx)], *n = &desktops[(currdeskidx = arg->i)];
    if (n->curr) XMapWindow(dis, n->curr->win);
    for (Client *c = n->head; c; c = c->next) XMapWindow(dis, c->win);
    XChangeWindowAttributes(dis, root, CWEventMask, &(XSetWindowAttributes){.do_not_propagate_mask = SubstructureNotifyMask});
    for (Client *c = d->head; c; c = c->next) if (c != d->curr) XUnmapWindow(dis, c->win);
    if (d->curr) XUnmapWindow(dis, d->curr->win);
    XChangeWindowAttributes(dis, root, CWEventMask, &(XSetWindowAttributes){.event_mask = ROOTMASK});
    if (n->head) { tile(n); focus(n->curr, n); }
    else focus(NULL, n);
    updatecurrentdesktop();
}

/**
 * remove all windows in all desktops by sending a delete window message
 */
void cleanup(void) {
    Window root_return, parent_return, *children;
    unsigned int nchildren;

    XUngrabKey(dis, AnyKey, AnyModifier, root);
    XQueryTree(dis, root, &root_return, &parent_return, &children, &nchildren);
    for (unsigned int i = 0; i < nchildren; i++) deletewindow(children[i]);
    if (children) XFree(children);
    XFreeCursor(dis, cur_norm);
    XFreeCursor(dis, cur_move);
    XFreeCursor(dis, cur_res);
    XUndefineCursor(dis, root);
    XDeleteProperty(dis, root, netatoms[NET_SUPPORTED]);
    XDeleteProperty(dis, root, netatoms[NET_CLIENT_LIST]);
    XDeleteProperty(dis, root, netatoms[NET_CLIENT_LIST_STACKING]);
    XDeleteProperty(dis, root, netatoms[NET_NUMBER_OF_DESKTOPS]);
    XDeleteProperty(dis, root, netatoms[NET_CURRENT_DESKTOP]);
    XDeleteProperty(dis, root, netatoms[NET_ACTIVE_WINDOW]);
    XDeleteProperty(dis, root, netatoms[NET_SUPPORTING_WM_CHECK]);
    XDeleteProperty(dis, supportwin, netatoms[NET_SUPPORTING_WM_CHECK]);
    XDeleteProperty(dis, supportwin, netatoms[NET_WM_NAME]);
    XDestroyWindow(dis, supportwin);
    XSync(dis, False);
}

/**
 * move the current focused client to another desktop
 *
 * add the current client as the last on the new desktop
 * then remove it from the current desktop
 */
void client_to_desktop(const Arg *arg) {
    if (arg->i == currdeskidx || arg->i < 0 || arg->i >= DESKTOPS || !desktops[currdeskidx].curr) return;
    Desktop *d = &desktops[currdeskidx], *n = &desktops[arg->i];
    Client *c = d->curr, *p = prevclient(d->curr, d), *l = prevclient(n->head, n);

    /* unlink current client from current desktop */
    if (d->head == c || !p) d->head = c->next; else p->next = c->next;
    c->next = NULL;
    XChangeWindowAttributes(dis, root, CWEventMask, &(XSetWindowAttributes){.do_not_propagate_mask = SubstructureNotifyMask});
    if (XUnmapWindow(dis, c->win)) focus(d->prev, d);
    XChangeWindowAttributes(dis, root, CWEventMask, &(XSetWindowAttributes){.event_mask = ROOTMASK});
    if (!(c->isfloat || c->istrans) || (d->head && !d->head->next)) tile(d);

    updateclientdesktop(c, arg->i);

    /* link client to new desktop and make it the current */
    focus(l ? (l->next = c):n->head ? (n->head->next = c):(n->head = c), n);

    if (follow_window) change_desktop(arg);

}

/**
 * receive and process client messages
 *
 * check if window wants to change its state to fullscreen,
 * or if the window want to become active/focused
 *
 * to change the state of a mapped window, a client MUST
 * send a _NET_WM_STATE client message to the root window
 * message_type must be _NET_WM_STATE
 *   data.l[0] is the action to be taken
 *   data.l[1] is the property to alter three actions:
 *   - remove/unset _NET_WM_STATE_REMOVE=0
 *   - add/set _NET_WM_STATE_ADD=1,
 *   - toggle _NET_WM_STATE_TOGGLE=2
 *
 * to request to become active, a client should send a
 * message of _NET_ACTIVE_WINDOW type. when such a message
 * is received and a client holding that window exists,
 * the window becomes the current active focused window
 * on its desktop.
 */
void clientmessage(XEvent *e) {
    Desktop *d = NULL; Client *c = NULL;
    if (e->xclient.message_type == netatoms[NET_WM_STATE]) {
        if ((unsigned)e->xclient.data.l[1] == netatoms[NET_WM_STATE_FULLSCREEN] || (unsigned)e->xclient.data.l[2] == netatoms[NET_WM_STATE_FULLSCREEN]) {
            if ((wintoclient(e->xclient.window, &c, &d))) {
                setfullscreen(c, d, (e->xclient.data.l[0] == 1 || (e->xclient.data.l[0] == 2 && !c->isfull)));
                if (!(c->isfloat || c->istrans) || !d->head->next) tile(d);
            }
        } else if ((unsigned)e->xclient.data.l[1] == netatoms[NET_WM_STATE_DEMANDS_ATTENTION] || (unsigned)e->xclient.data.l[2] == netatoms[NET_WM_STATE_DEMANDS_ATTENTION]) {
            if ((wintoclient(e->xclient.window, &c, &d)))
                c->isurgn = (c != desktops[currdeskidx].curr && (e->xclient.data.l[0] == 1 || (e->xclient.data.l[0] == 2 && !c->isurgn)));
        }
    } else if (e->xclient.message_type == netatoms[NET_ACTIVE_WINDOW]) {
        if (wintoclient(e->xclient.window, &c, &d))
            focus(c, d);
    } else if (e->xclient.message_type == netatoms[NET_CLOSE_WINDOW]) deletewindow(e->xclient.window);
    else if (e->xclient.message_type == netatoms[NET_CURRENT_DESKTOP])
        change_desktop(&(Arg){.i = e->xclient.data.l[0]});
}

void configure(Client *c) {
    XConfigureEvent ce;

    ce.type = ConfigureNotify;
    ce.display = dis;
    ce.event = c->win;
    ce.window = c->win;
    ce.x = c->x;
    ce.y = c->y;
    ce.width = c->w;
    ce.height = c->h;
    ce.border_width = c->bw;
    ce.above = None;
    ce.override_redirect = False;
    XSendEvent(dis, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

/**
 * configure a window's size, position, border width, and stacking order.
 *
 * windows usually have a prefered size (width, height) and position (x, y),
 * and sometimes border width and stacking order (above, detail).
 * a configure request attempts to reconfigure those properties for a window.
 *
 * we don't really care about those values, because a tiling wm will impose
 * its own values for those properties.
 * however the requested values must be set initially for some windows,
 * otherwise the window will misbehave or even crash (see gedit, geany, gvim).
 *
 * some windows depend on the number of columns and rows to set their
 * size, and not on pixels (terminals, consoles, some editors etc).
 * normally those clients when tiled and respecting the prefered size
 * will create gaps around them (window_hints).
 * however, clients are tiled to match the wm's prefered size,
 * not respecting those prefered values.
 *
 * some windows implement window manager functions themselves.
 * that is windows explicitly steal focus, or manage subwindows,
 * or move windows around w/o the window manager's help, etc..
 * to disallow this behavior, we 'tile()' the desktop to which
 * the window that sent the configure request belongs.
 */
void configurerequest(XEvent *e) {
    Client *c = NULL; Desktop *d = NULL;
    XConfigureRequestEvent *ev = &e->xconfigurerequest;
    XWindowChanges wc;

    if (wintoclient(ev->window, &c, &d)) {
        if (ev->value_mask & CWBorderWidth)
            c->bw = ev->border_width;
        else if (c->isfloat || d->mode == FLOAT) {
            if (ev->value_mask & CWX) {
                c->oldx = c->x;
                c->x = ev->x;
            }
            if (ev->value_mask & CWY) {
                c->oldy = c->y;
                c->y = ev->y;
            }
            if (ev->value_mask & CWWidth) {
                c->oldw = c->w;
                c->w = ev->width;
            }
            if (ev->value_mask & CWHeight) {
                c->oldh = c->h;
                c->h = ev->height;
            }
            if ((c->x + c->w) > ww && c->isfloat)
                c->x = ww / 2 - HEIGHT(c) / 2; /* center in x direction */
            if ((c->y + c->h) > wh && c->isfloat)
                c->y = wh / 2 - WIDTH(c) / 2; /* center in y direction */
            if ((ev->value_mask & (CWX|CWY)) && !(ev->value_mask & (CWWidth|CWHeight)))
                configure(c);
            if (d == &desktops[currdeskidx])
                XMoveResizeWindow(dis, c->win, c->x, c->y, c->w, c->h);
        } else configure(c);
    } else {
        wc.x = ev->x;
        wc.y = ev->y;
        wc.width = ev->width;
        wc.height = ev->height;
        wc.border_width = ev->border_width;
        wc.sibling = ev->above;
        wc.stack_mode = ev->detail;
        XConfigureWindow(dis, ev->window, ev->value_mask, &wc);
    }
    XSync(dis, False);
}

/**
 * clients receiving a WM_DELETE_WINDOW message should behave as if
 * the user selected "delete window" from a hypothetical menu and
 * also perform any confirmation dialog with the user.
 */
void deletewindow(Window w) {
    XEvent ev = { .type = ClientMessage };
    ev.xclient.window = w;
    ev.xclient.format = 32;
    ev.xclient.message_type = wmatoms[WM_PROTOCOLS];
    ev.xclient.data.l[0]    = wmatoms[WM_DELETE_WINDOW];
    ev.xclient.data.l[1]    = CurrentTime;
    XSendEvent(dis, w, False, NoEventMask, &ev);
}

/**
 * generated whenever a client application destroys a window
 *
 * a destroy notification is received when a window is being closed
 * on receival, remove the client that held that window
 */
void destroynotify(XEvent *e) {
    Desktop *d = NULL; Client *c = NULL;
    if (wintoclient(e->xdestroywindow.window, &c, &d)) removeclient(c, d);
}

/**
 * when the mouse enters a window's borders, that window,
 * if has set notifications of such events (EnterWindowMask)
 * will notify that the pointer entered its region
 * and will get focus if follow_mouse is set in the config.
 */
void enternotify(XEvent *e) {
    Desktop *d = NULL; Client *c = NULL, *p = NULL;

    if (!follow_mouse || (e->xcrossing.mode != NotifyNormal && e->xcrossing.detail == NotifyInferior)
        || !wintoclient(e->xcrossing.window, &c, &d) || e->xcrossing.window == d->curr->win) return;

    if ((p = d->prev))
        XChangeWindowAttributes(dis, p->win, CWEventMask, &(XSetWindowAttributes){.do_not_propagate_mask = EnterWindowMask});
    focus(c, d);
    if (p) XChangeWindowAttributes(dis, p->win, CWEventMask, &(XSetWindowAttributes){.event_mask = EnterWindowMask});
}

/**
 * 1. set current/active/focused and previously focused client
 *    in other words, manage curr and prev references
 * 2. restack clients
 * 3. highlight borders and set active window property
 * 4. give input focus to the current/active/focused client
 */
void focus(Client *c, Desktop *d) {
    /* update references to prev and curr,
     * previously focused and currently focused clients.
     *
     * if there are no clients (!head) or the new client
     * is NULL, then delete the _NET_ACTIVE_WINDOW property
     *
     * if the new client is the prev client then
     *  - either the current client was removed
     *    and thus focus(prev) was called
     *  - or the previous from current is prev
     *    ie, two consecutive clients were focused
     *    and then prev_win() was called, to focus
     *    the previous from current client, which
     *    happens to be prev (curr == c->next).
     * (below: h:head p:prev c:curr)
     *
     * [h]->[p]->[c]->NULL   ===>   [h|p]->[c]->NULL
     *            ^ remove current
     *
     * [h]->[p]->[c]->NULL   ===>   [h]->[c]->[p]->NULL
     *       ^ prev_win swaps prev and curr
     *
     * in the first case we need to update prev reference,
     * choice here is to set it to the previous from the
     * new current client.
     * the second case is handled as any other case, the
     * current client is now the previously focused (prev = curr)
     * and the new current client is now curr (curr = c)
     *
     * references should only change when the current
     * client is different from the one given to focus.
     *
     * the new client should never be NULL, except if,
     * there is no other client on the workspace (!head).
     * prev and curr always point to different clients.
     *
     * NOTICE: remove client can remove any client,
     * not just the current (curr). Thus, if prev is
     * removed, its reference needs to be updated.
     * That is handled by removeclient() function.
     * All other reference changes for curr and prev
     * should be and are handled here.
     */
    if (!d->head || !c) { /* no clients - no active window - focus root window */
        XDeleteProperty(dis, root, netatoms[NET_ACTIVE_WINDOW]);
        d->curr = d->prev = NULL;
        return;
    } else if (d->prev == c && d->curr != c->next) { d->prev = prevclient((d->curr = c), d);
    } else if (d->curr != c) { d->prev = d->curr; d->curr = c; }
    if (c->isurgn) c->isurgn = False;

    /* restack clients
     *
     * stack order is based on client properties.
     * from top to bottom:
     *  - current when floating or transient
     *  - floating or trancient windows
     *  - current when tiled
     *  - current when fullscreen
     *  - fullscreen windows
     *  - tiled windows
     *
     * num of n:all fl:fullscreen ft:floating/transient windows
     */
    int n = 0, fl = 0, ft = 0;
    for (c = d->head; c; c = c->next, ++n) if (ISFFT(c)) { fl++; if (!c->isfull) ft++; }
    Window w[n];
    w[(d->curr->isfloat || d->curr->istrans) ? 0:ft] = d->curr->win;
    for (fl += !ISFFT(d->curr) ? 1:0, c = d->head; c; c = c->next) {
        XSetWindowBorder(dis, c->win, c == d->curr ? win_focus:win_unfocus);
        /*
         * a window should have borders in any case, except if
         *  - the window is fullscreen
         *  - the window is not floating or transient and
         *      - the mode is MONOCLE or,
         *      - it is the only window on screen
         */
        /*XSetWindowBorderWidth(dis, c->win, (!ISFFT(c) && (d->mode == MONOCLE || !d->head->next)) ? 0:c->bw);*/
        if (c != d->curr) w[c->isfull ? --fl:ISFFT(c) ? --ft:--n] = c->win;
        if (c == d->curr) grabbuttons(c);
    }
    XRestackWindows(dis, w, LENGTH(w));

    XSetInputFocus(dis, d->curr->win, RevertToPointerRoot, CurrentTime);
    XChangeProperty(dis, root, netatoms[NET_ACTIVE_WINDOW], XA_WINDOW, 32,
            PropModeReplace, (unsigned char *)&d->curr->win, 1);
    updatecurrentdesktop();

    XSync(dis, False);
}

/**
 * dont give focus to any client except current.
 * some apps explicitly call XSetInputFocus (see
 * tabbed, chromium), resulting in loss of input
 * focuse (mouse/kbd) from the current focused
 * client.
 *
 * this gives focus back to the current selected
 * client, by the user, through the wm.
 */
void focusin(XEvent *e) {
    Desktop *d = &desktops[currdeskidx];
    if (d->curr && d->curr->win != e->xfocus.window) focus(d->curr, d);
}

/**
 * find and focus the first client that received an urgent hint
 * first look in the current desktop then on other desktops
 */
void focusurgent(void) {
    Client *c = NULL;
    int d = -1;
    for (c = desktops[currdeskidx].head; c && !c->isurgn; c = c->next);
    while (!c && d < DESKTOPS-1) for (c = desktops[++d].head; c && !c->isurgn; c = c->next);
    if (c) { if (d != -1) change_desktop(&(Arg){.i = d}); focus(c, &desktops[currdeskidx]); }
}

/**
 * get a pixel with the requested color to
 * fill some window area (such as borders)
 */
unsigned long getcolor(const char* color, const int screen) {
    XColor c; Colormap map = DefaultColormap(dis, screen);
    if (!XAllocNamedColor(dis, map, color, &c, &c)) err(EXIT_FAILURE, "cannot allocate color");
    return c.pixel;
}

/**
 * register button bindings to be notified of
 * when they occur.
 * the wm listens to those button bindings and
 * calls an appropriate handler when a binding
 * occurs (see buttonpress).
 */
void grabbuttons(Client *c) {
    unsigned int b, m, modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };

    for (b = 0, m = 0; b < LENGTH(buttons); b++, m = 0)
        if (buttons[b].click == CLIENTWIN)
            while (m < LENGTH(modifiers))
                XGrabButton(dis, buttons[b].button, buttons[b].mask|modifiers[m++], c->win,
                        False, BUTTONMASK, GrabModeAsync, GrabModeAsync, None, None);
}

/**
 * register key bindings to be notified of
 * when they occur.
 * the wm listens to those key bindings and
 * calls an appropriate handler when a binding
 * occurs (see keypressed).
 */
void grabkeys(void) {
    KeyCode code;
    XUngrabKey(dis, AnyKey, AnyModifier, root);

    unsigned int k, m, modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
    for (k = 0, m = 0; k < LENGTH(keys); k++, m = 0)
        while ((code = XKeysymToKeycode(dis, keys[k].keysym)) && m < LENGTH(modifiers))
            XGrabKey(dis, code, keys[k].mod|modifiers[m++], root, True, GrabModeAsync, GrabModeAsync);
}

/**
 * grid mode / grid layout
 * arrange windows in a grid aka fair
 */
void grid(int x, int y, int w, int h, const Desktop *d) {
    int n = 0, cols = 0, cn = 0, rn = 0, i = -1;
    for (Client *c = d->head; c; c = c->next) if (!ISFFT(c)) ++n;
    for (cols = 0; cols <= n/2; cols++) if (cols*cols >= n) break; /* emulate square root */
    if (n == 0) return; else if (n == 5) cols = 2;

    int rows = n/cols, ch = h - uselessgap, cw = (w - uselessgap)/(cols ? cols:1);
    for (Client *c = d->head; c; c = c->next) {
        if (ISFFT(c)) continue; else ++i;
        if (i/rows + 1 > cols - n%cols) rows = n/cols + 1;
        resizeclient(c, x + cn*cw + uselessgap, y + rn*ch/rows + uselessgap,
                cw - 2*c->bw - uselessgap, ch/rows - 2*c->bw - uselessgap);
        if (++rn >= rows) { rn = 0; cn++; }
    }
}

/**
 * on the press of a key binding (see grabkeys)
 * call the appropriate handler
 */
void keypress(XEvent *e) {
    KeySym keysym = XkbKeycodeToKeysym(dis, e->xkey.keycode, 0, 0);
    for (unsigned int i = 0; i < LENGTH(keys); i++)
        if (keysym == keys[i].keysym && CLEANMASK(keys[i].mod) == CLEANMASK(e->xkey.state))
            if (keys[i].func) keys[i].func(&keys[i].arg);
}

/**
 * explicitly kill the current client - close the highlighted window
 * if the client accepts WM_DELETE_WINDOW requests send a delete message
 * otherwise forcefully kill and remove the client
 */
void killclient(void) {
    Desktop *d = &desktops[currdeskidx];
    if (!d->curr) return;

    Atom *prot = NULL; int n = -1;
    if (XGetWMProtocols(dis, d->curr->win, &prot, &n))
        while (--n >= 0 && prot[n] != wmatoms[WM_DELETE_WINDOW]);
    if (n < 0) { XKillClient(dis, d->curr->win); removeclient(d->curr, d); }
    else deletewindow(d->curr->win);
    if (prot) XFree(prot);
}

/**
 * focus the previously/last focused desktop
 */
void last_desktop(void) {
    change_desktop(&(Arg){.i = prevdeskidx});
}

/**
 * a map request is received when a window wants to display itself.
 * if the window has override_redirect flag set,
 * then it should not be handled by the wm.
 * if the window already has a client then there is nothing to do.
 *
 * match window class and/or install name against an app rule.
 * create a new client for the window and add it to the appropriate desktop.
 * set the floating, transient and fullscreen state of the client.
 * if the desktop in which the window is to be spawned is the current desktop
 * then display/map the window, else, if follow is set, focus the new desktop.
 */
void maprequest(XEvent *e) {
    Desktop *d = NULL; Client *c = NULL;
    Window w = e->xmaprequest.window;
    XTextProperty name;
    XClassHint ch = {0, 0};
    Bool follow = False, floating = False, aside = False;
    int newdsk = currdeskidx, i;
    unsigned long l;
    unsigned char *state = NULL, *type = NULL;
    Atom a;
    XWindowChanges wc;
    XWindowAttributes wa = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    if (wintoclient(w, &c, &d) || (XGetWindowAttributes(dis, w, &wa) && wa.override_redirect)) return;

    if (XGetWindowProperty(dis, w, netatoms[NET_WM_WINDOW_TYPE], 0L, sizeof a,
            False, XA_ATOM, &a, &i, &l, &l, &type) == Success && type)
        if (*(Atom *)type == netatoms[NET_WM_WINDOW_TYPE_DOCK] || *(Atom *)type == netatoms[NET_WM_WINDOW_TYPE_DESKTOP]) {
            XMapWindow(dis, w);
	        if (type) XFree(type);
            return;
        }

    if ((XGetTextProperty(dis, w, &name, netatoms[NET_WM_NAME]) || XGetTextProperty(dis, w, &name, XA_WM_NAME)) && XGetClassHint(dis, w, &ch)) {
        for (unsigned int i = 0; i < LENGTH(rules); i++)
            if ((!rules[i].title || strstr((char *)name.value, rules[i].title))
                    && (!rules[i].class || strstr(ch.res_class, rules[i].class))
                    && (!rules[i].instance || strstr(ch.res_name, rules[i].instance))) {
                if (rules[i].desktop >= 0 && rules[i].desktop < DESKTOPS) newdsk = rules[i].desktop;
                follow = rules[i].follow, floating = rules[i].floating, aside = rules[i].attachaside;
            }
    }
    if (ch.res_class) XFree(ch.res_class);
    if (ch.res_name) XFree(ch.res_name);
    if (name.value) XFree(name.value);

    c = addwindow(w, (d = &desktops[newdsk]), aside); /* from now on, use c->win */
    c->x = c->oldx = wa.x;
    c->y = c->oldy = wa.y;
    c->w = c->oldw = wa.width;
    c->h = c->oldh = wa.height;
    c->bw = borderwidth;
    wc.border_width = c->bw;
    XConfigureWindow(dis, w, CWBorderWidth, &wc);
    configure(c);
    updatesizehints(c);
    /*setclientstate(c, NormalState);*/
    c->istrans = XGetTransientForHint(dis, c->win, &w);
    if ((c->isfloat = (c->isfixed || floating || d->mode == FLOAT)) && !c->istrans)
        XMoveWindow(dis, c->win, (ww - wa.width)/2, (wh - wa.height)/2);

    if (XGetWindowProperty(dis, c->win, netatoms[NET_WM_WINDOW_TYPE], 0L, sizeof a,
            False, XA_ATOM, &a, &i, &l, &l, &type) == Success && type)
        if (*(Atom *)type == netatoms[NET_WM_WINDOW_TYPE_DIALOG] || *(Atom *)type == netatoms[NET_WM_WINDOW_TYPE_SPLASH]
                || *(Atom *)type == netatoms[NET_WM_WINDOW_TYPE_UTILITY] || *(Atom *)type == netatoms[NET_WM_WINDOW_TYPE_MENU])
            c->isfloat = True;
    if (type) XFree(type);

    if (XGetWindowProperty(dis, c->win, netatoms[NET_WM_STATE], 0L, sizeof a,
            False, XA_ATOM, &a, &i, &l, &l, &state) == Success && state) {
        if (*(Atom *)state == netatoms[NET_WM_STATE_FULLSCREEN])
            setfullscreen(c, d, True);
        else if (*(Atom *)state == netatoms[NET_WM_STATE_ABOVE])
            c->isfloat = True;
    }
    if (state) XFree(state);

    if (currdeskidx == newdsk) { if (!ISFFT(c)) tile(d); XMapWindow(dis, c->win); }
    else if (follow) change_desktop(&(Arg){.i = newdsk});
    XChangeProperty(dis, root, netatoms[NET_CLIENT_LIST], XA_WINDOW, 32,
            PropModeAppend, (unsigned char *)&(c->win), 1);
    XChangeProperty(dis, root, netatoms[NET_CLIENT_LIST_STACKING], XA_WINDOW, 32,
            PropModeAppend, (unsigned char *)&(c->win), 1);
    updateclientdesktop(c, newdsk);
    focus(c, d);
}

/**
 * handle resize and positioning of a window with the pointer.
 *
 * grab the pointer and get it's current position.
 * now, all pointer movement events will be reported until it is ungrabbed.
 *
 * while the mouse is pressed, grab interesting events (see button press,
 * button release, pointer motion).
 * on on pointer movement resize or move the window under the curson.
 * also handle map requests and configure requests.
 *
 * finally, on ButtonRelease, ungrab the poitner.
 * event handling is passed back to run() function.
 *
 * once a window has been moved or resized, it's marked as floating.
 */
void mousemotion(const Arg *arg) {
    Desktop *d = &desktops[currdeskidx];
    XWindowAttributes wa;
    XEvent ev;

    if (!d->curr || !XGetWindowAttributes(dis, d->curr->win, &wa)) return;

    if (arg->i == RESIZE) {
        XWarpPointer(dis, d->curr->win, d->curr->win, 0, 0, 0, 0, --wa.width, --wa.height);
        if (XGrabPointer(dis, root, False, BUTTONMASK|PointerMotionMask, GrabModeAsync,
                GrabModeAsync, None, cur_res, CurrentTime) != GrabSuccess) return;
    } else if (arg->i == MOVE)
        if (XGrabPointer(dis, root, False, BUTTONMASK|PointerMotionMask, GrabModeAsync,
                GrabModeAsync, None, cur_move, CurrentTime) != GrabSuccess) return;

    int rx, ry, c, xw, yh; unsigned int v; Window w;
    if (!XQueryPointer(dis, root, &w, &w, &rx, &ry, &c, &c, &v) || w != d->curr->win) return;

    if (!d->curr->isfloat && !d->curr->istrans) { d->curr->isfloat = True; tile(d); focus(d->curr, d); }

    do {
        XMaskEvent(dis, BUTTONMASK|PointerMotionMask|SubstructureRedirectMask, &ev);
        if (ev.type == MotionNotify) {
            xw = (arg->i == MOVE ? wa.x:wa.width)  + ev.xmotion.x - rx;
            yh = (arg->i == MOVE ? wa.y:wa.height) + ev.xmotion.y - ry;
            if (arg->i == RESIZE) resize(d->curr, d->curr->x, d->curr->y, xw, yh, True);
            else if (arg->i == MOVE) resize(d->curr, xw, yh, d->curr->w, d->curr->h, True);
        } else if (ev.type == ConfigureRequest || ev.type == MapRequest) events[ev.type](&ev);
    } while (ev.type != ButtonRelease);

    XUngrabPointer(dis, CurrentTime);
}

/**
 * monocle aka max aka fullscreen mode/layout
 * each window should cover all the available screen space
 */
void monocle(int x, int y, int w, int h, const Desktop *d) {
    for (Client *c = d->head; c; c = c->next) if (!ISFFT(c)) resize(c, x, y, w - 2*c->bw, h - 2*c->bw, False);
}

/**
 * swap positions of current and next from current clients
 */
void move_down(void) {
    Desktop *d = &desktops[currdeskidx];
    if (!d->curr || !d->head->next) return;
    /* p is previous, c is current, n is next, if current is head n is last */
    Client *p = prevclient(d->curr, d), *n = (d->curr->next) ? d->curr->next:d->head;
    /*
     * if c is head, swapping with n should update head to n
     * [c]->[n]->..  ==>  [n]->[c]->..
     *  ^head              ^head
     *
     * else there is a previous client and p->next should be what's after c
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     */
    if (d->curr == d->head) d->head = n; else p->next = d->curr->next;
    /*
     * if c is the last client, c will be the current head
     * [n]->..->[p]->[c]->NULL  ==>  [c]->[n]->..->[p]->NULL
     *  ^head                         ^head
     * else c will take the place of n, so c-next will be n->next
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     */
    d->curr->next = (d->curr->next) ? n->next:n;
    /*
     * if c was swapped with n then they now point to the same ->next. n->next should be c
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     *                                        [c]-^
     *
     * else c is the last client and n is head,
     * so c will be move to be head, no need to update n->next
     * [n]->..->[p]->[c]->NULL  ==>  [c]->[n]->..->[p]->NULL
     *  ^head                         ^head
     */
    if (d->curr->next == n->next) n->next = d->curr; else d->head = d->curr;
    if (!d->curr->isfloat && !d->curr->istrans) tile(d);
}

/**
 * swap positions of current and previous from current clients
 */
void move_up(void) {
    Desktop *d = &desktops[currdeskidx];
    if (!d->curr || !d->head->next) return;
    /* p is previous from current or last if current is head */
    Client *pp = NULL, *p = prevclient(d->curr, d);
    /* pp is previous from p, or null if current is head and thus p is last */
    if (p->next) for (pp = d->head; pp && pp->next != p; pp = pp->next);
    /*
     * if p has a previous client then the next client should be current (current is c)
     * ..->[pp]->[p]->[c]->..  ==>  ..->[pp]->[c]->[p]->..
     *
     * if p doesn't have a previous client, then p might be head, so head must change to c
     * [p]->[c]->..  ==>  [c]->[p]->..
     *  ^head              ^head
     * if p is not head, then c is head (and p is last), so the new head is next of c
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    if (pp) pp->next = d->curr; else d->head = (d->curr == d->head) ? d->curr->next:d->curr;
    /*
     * next of p should be next of c
     * ..->[pp]->[p]->[c]->[n]->..  ==>  ..->[pp]->[c]->[p]->[n]->..
     * except if c was head (now c->next is head), so next of p should be c
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    p->next = (d->curr->next == d->head) ? d->curr:d->curr->next;
    /*
     * next of c should be p
     * ..->[pp]->[p]->[c]->[n]->..  ==>  ..->[pp]->[c]->[p]->[n]->..
     * except if c was head (now c->next is head), so c is must be last
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    d->curr->next = (d->curr->next == d->head) ? NULL:p;
    if (!d->curr->isfloat && !d->curr->istrans) tile(d);
}

/**
 * move and resize a window with the keyboard
 */
void moveresize(const Arg *arg) {
    Desktop *d = &desktops[currdeskidx];
    XWindowAttributes wa;
    if (!d->curr || !XGetWindowAttributes(dis, d->curr->win, &wa)) return;
    if (!d->curr->isfloat && !d->curr->istrans) { d->curr->isfloat = True; tile(d); focus(d->curr, d); }
    resizeclient(d->curr, wa.x + ((int *)arg->v)[0], wa.y + ((int *)arg->v)[1],
            wa.width + ((int *)arg->v)[2], wa.height + ((int *)arg->v)[3]);
}

/**
 * cyclic focus the next window
 * if the window is the last on stack, focus head
 */
void next_win(void) {
    Desktop *d = &desktops[currdeskidx];
    if (d->curr && d->head->next) focus(d->curr->next ? d->curr->next:d->head, d);
}

/**
 * increase or decrease the number
 * of windows in the master area
 */
void nmaster(const Arg *arg) {
    Desktop *d = &desktops[currdeskidx];
    if ((d->nm += arg->i) >= 1) tile(d); else d->nm -= arg->i;
}

/**
 * get the previous client from the given
 * if no such client, return NULL
 */
Client* prevclient(Client *c, Desktop *d) {
    Client *p = NULL;
    if (c && d->head && d->head->next) for (p = d->head; p->next && p->next != c; p = p->next);
    return p;
}

/**
 * cyclic focus the previous window
 * if the window is head, focus the last stack window
 */
void prev_win(void) {
    Desktop *d = &desktops[currdeskidx];
    if (d->curr && d->head->next) focus(prevclient(d->curr, d), d);
}

/**
 * set urgent hint for a window
 */
void propertynotify(XEvent *e) {
    Desktop *d = NULL; Client *c = NULL;
    if (!wintoclient(e->xproperty.window, &c, &d)) return;

    if (e->xproperty.atom == XA_WM_HINTS) {
        XWMHints *wmh = XGetWMHints(dis, c->win);
        c->isurgn = (c != desktops[currdeskidx].curr && (wmh && (wmh->flags & XUrgencyHint)));
        if (wmh) XFree(wmh);
    } else if (e->xproperty.atom == XA_WM_NORMAL_HINTS) updatesizehints(c);
}

/**
 * to quit just stop receiving events
 * run is stopped and control is back to main
 */
void quit(const Arg *arg) {
    retval = arg->i;
    running = False;
}

/**
 * remove the specified client from the given desktop
 *
 * if c was the previous client, previous must be updated.
 * if c was the current client, current must be updated.
 */
void removeclient(Client *c, Desktop *d) {
    Client **p = NULL;
    for (p = &d->head; *p && (*p != c); p = &(*p)->next);
    if (!*p) return; else *p = c->next;
    if (c == d->prev && !(d->prev = prevclient(d->curr, d))) d->prev = d->head;
    if (c == d->curr || (d->head && !d->head->next)) focus(d->prev, d);
    if (!(c->isfloat || c->istrans) || (d->head && !d->head->next)) tile(d);
    /*setclientstate(c, WithdrawnState);*/
    free(c);
    updateclientlist();
}

void resize(Client *c, int x, int y, int w, int h, Bool interact) {
    if (applysizehints(c, &x, &y, &w, &h, interact)) resizeclient(c, x, y, w, h);
}

void resizeclient(Client *c, int x, int y, int w, int h) {
    XWindowChanges wc;

    c->oldx = c->x; c->x = wc.x = x;
    c->oldy = c->y; c->y = wc.y = y;
    c->oldw = c->w; c->w = wc.width = w;
    c->oldh = c->h; c->h = wc.height = h;
    wc.border_width = c->bw;
    XConfigureWindow(dis, c->win, CWX|CWY|CWWidth|CWHeight|CWBorderWidth, &wc);
    configure(c);
    XSync(dis, False);
}

/**
 * resize the master size
 * we should check for window size limits for both master and
 * stack clients. the size of a window can't be less than minwsz
 */
void resize_master(const Arg *arg) {
    Desktop *d = &desktops[currdeskidx];
    unsigned int msz = (d->mode == BSTACK ? wh:ww) * d->mfact + (d->masz += arg->i);
    if (msz >= minwsz && (d->mode == BSTACK ? wh:ww) - msz >= minwsz + uselessgap) tile(d);
    else d->masz -= arg->i; /* reset master area size */
}

/**
 * resize the first stack window
 */
void resize_stack(const Arg *arg) {
    desktops[currdeskidx].sasz += arg->i;
    tile(&desktops[currdeskidx]);
}

/**
 * jump and focus the next or previous desktop
 */
void rotate(const Arg *arg) {
    change_desktop(&(Arg){.i = (DESKTOPS + currdeskidx + arg->i) % DESKTOPS});
}

/**
 * jump and focus the next non-empty desktop
 */
void rotate_filled(const Arg *arg) {
    int n = arg->i;
    while (n < DESKTOPS && !desktops[(DESKTOPS + currdeskidx + n) % DESKTOPS].head) (n += arg->i);
    change_desktop(&(Arg){.i = (DESKTOPS + currdeskidx + n) % DESKTOPS});
}

/**
 * main event loop
 * on receival of an event call the appropriate handler
 */
void run(void) {
    XEvent ev;
    while (running && !XNextEvent(dis, &ev)) if (events[ev.type]) events[ev.type](&ev);
}

void setclientstate(Client *c, long state) {
    long data[] = { state, None };

    XChangeProperty(dis, c->win, wmatoms[WM_STATE], wmatoms[WM_STATE], 32,
            PropModeReplace, (unsigned char *)data, 2);
}

/**
 * set the EWMH desktop names
 */
void setdesktopnames(void) {
    char buf[1024], *pos;
    int len = 0;

    pos = buf;
    for (unsigned int i = 0; i < DESKTOPS; i++) {
        snprintf(pos, strlen(desktops[i].name) + 1, "%s", desktops[i].name);
        pos += (strlen(desktops[i].name) + 1);
    }
    len = pos - buf;

    XChangeProperty(dis, root, netatoms[NET_DESKTOP_NAMES], netatoms[UTF8_STRING], 8,
            PropModeReplace, (unsigned char *)buf, len);
}

/**
 * set the fullscreen state of a client
 *
 * if a client gets fullscreen resize it
 * to cover all screen space.
 * the border should be zero (0).
 *
 * if a client is reset from fullscreen,
 * the border should be borderwidth,
 * except if no other client is on that desktop.
 */
void setfullscreen(Client *c, Desktop *d, Bool fullscreen) {
    if (fullscreen) {
        XChangeProperty(dis, c->win, netatoms[NET_WM_STATE], XA_ATOM, 32,
                PropModeReplace, (unsigned char*)&netatoms[NET_WM_STATE_FULLSCREEN], 1);
        c->isfull = True;
        c->oldbw = c->bw;
        c->bw = 0;
        c->isfloat = True;
        resizeclient(c, 0, 0, ww, wh + panelheight);
    } else {
        XChangeProperty(dis, c->win, netatoms[NET_WM_STATE], XA_ATOM, 32,
                PropModeReplace, (unsigned char*)0, 0);
        c->isfull = False;
        c->bw = c->oldbw;
        c->isfloat = False;
        c->x = c->oldx; c->y = c->oldy; c->w = c->oldw; c->h = c->oldh;
        resizeclient(c, c->x, c->y, c->w, c->h);
        tile(d);
    }
}

/**
 * set the EWMH number of desktops
 */
void setnumberofdesktops(void) {
    long data[] = { DESKTOPS };

    XChangeProperty(dis, root, netatoms[NET_NUMBER_OF_DESKTOPS], XA_CARDINAL, 32,
            PropModeReplace, (unsigned char *)data, 1);
}

/**
 * set initial values
 */
void setup(void) {
    XSetWindowAttributes wa;

    sigchld(0);

    /* screen and root window */
    const int screen = DefaultScreen(dis);
    root = RootWindow(dis, screen);

    /* screen width and height */
    ww = XDisplayWidth(dis,  screen) - (panelhoriz ? 0 : panelheight);
    wh = XDisplayHeight(dis, screen) - (panelhoriz ? panelheight : 0);

    /* init cursors */
    cur_norm = XCreateFontCursor(dis, XC_left_ptr);
    cur_move = XCreateFontCursor(dis, XC_fleur);
    cur_res = XCreateFontCursor(dis, XC_sizing);
    XDefineCursor(dis, root, cur_norm);

    /* initialize each desktop */
    for (unsigned int d = 0; d < DESKTOPS; d++)
        desktops[d] = (Desktop){ .name = desksettings[d].name, .mode = desksettings[d].mode,
                .mfact = desksettings[d].mfact, .nm = desksettings[d].nm, .sbar = desksettings[d].sbar };

    /* get colors for client borders */
    win_focus = getcolor(focuscolor, screen);
    win_unfocus = getcolor(unfocuscolor, screen);

    /* set numlockmask */
    XModifierKeymap *modmap = XGetModifierMapping(dis);
    for (int k = 0; k < 8; k++) for (int j = 0; j < modmap->max_keypermod; j++)
        if (modmap->modifiermap[modmap->max_keypermod*k + j] == XKeysymToKeycode(dis, XK_Num_Lock))
            numlockmask = (1 << k);
    XFreeModifiermap(modmap);

    /* set up atoms */
    wmatoms[WM_PROTOCOLS]                    = XInternAtom(dis, "WM_PROTOCOLS",                    False);
    wmatoms[WM_DELETE_WINDOW]                = XInternAtom(dis, "WM_DELETE_WINDOW",                False);
    wmatoms[WM_STATE]                        = XInternAtom(dis, "WM_STATE",                        False);
    netatoms[NET_ACTIVE_WINDOW]              = XInternAtom(dis, "_NET_ACTIVE_WINDOW",              False);
    netatoms[NET_CLOSE_WINDOW]               = XInternAtom(dis, "_NET_CLOSE_WINDOW",               False);
    netatoms[NET_SUPPORTED]                  = XInternAtom(dis, "_NET_SUPPORTED",                  False);
    netatoms[NET_SUPPORTING_WM_CHECK]        = XInternAtom(dis, "_NET_SUPPORTING_WM_CHECK",        False);
    netatoms[NET_WM_NAME]                    = XInternAtom(dis, "_NET_WM_NAME",                    False);
    netatoms[NET_CLIENT_LIST]                = XInternAtom(dis, "_NET_CLIENT_LIST",                False);
    netatoms[NET_CLIENT_LIST_STACKING]       = XInternAtom(dis, "_NET_CLIENT_LIST_STACKING",       False);
    netatoms[NET_NUMBER_OF_DESKTOPS]         = XInternAtom(dis, "_NET_NUMBER_OF_DESKTOPS",         False);
    netatoms[NET_CURRENT_DESKTOP]            = XInternAtom(dis, "_NET_CURRENT_DESKTOP",            False);
    netatoms[NET_DESKTOP_NAMES]              = XInternAtom(dis, "_NET_DESKTOP_NAMES",              False);
    netatoms[NET_WM_DESKTOP]                 = XInternAtom(dis, "_NET_WM_DESKTOP",                 False);
    netatoms[NET_WM_STATE]                   = XInternAtom(dis, "_NET_WM_STATE",                   False);
    netatoms[NET_WM_STATE_ABOVE]             = XInternAtom(dis, "_NET_WM_STATE_ABOVE",             False);
    netatoms[NET_WM_STATE_FULLSCREEN]        = XInternAtom(dis, "_NET_WM_STATE_FULLSCREEN",        False);
    netatoms[NET_WM_STATE_DEMANDS_ATTENTION] = XInternAtom(dis, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
    netatoms[NET_WM_WINDOW_TYPE]             = XInternAtom(dis, "_NET_WM_WINDOW_TYPE",             False);
    netatoms[NET_WM_WINDOW_TYPE_DOCK]        = XInternAtom(dis, "_NET_WM_WINDOW_TYPE_DOCK",        False);
    netatoms[NET_WM_WINDOW_TYPE_DESKTOP]     = XInternAtom(dis, "_NET_WM_WINDOW_TYPE_DESKTOP",     False);
    netatoms[NET_WM_WINDOW_TYPE_SPLASH]      = XInternAtom(dis, "_NET_WM_WINDOW_TYPE_SPLASH",      False);
    netatoms[NET_WM_WINDOW_TYPE_MENU]        = XInternAtom(dis, "_NET_WM_WINDOW_TYPE_MENU",        False);
    netatoms[NET_WM_WINDOW_TYPE_DIALOG]      = XInternAtom(dis, "_NET_WM_WINDOW_TYPE_DIALOG",      False);
    netatoms[NET_WM_WINDOW_TYPE_UTILITY]     = XInternAtom(dis, "_NET_WM_WINDOW_TYPE_UTILITY",     False);
    netatoms[UTF8_STRING]                    = XInternAtom(dis, "UTF8_STRING",                     False);

    XChangeProperty(dis, root, netatoms[NET_SUPPORTED], XA_ATOM, 32,
            PropModeReplace, (unsigned char *)netatoms, NET_COUNT);

    wa.override_redirect = True;
    supportwin = XCreateWindow(dis, root, -100, 0, 1, 1, 0, DefaultDepth(dis, screen), CopyFromParent,
            DefaultVisual(dis, screen), CWOverrideRedirect, &wa);
    XChangeProperty(dis, supportwin, netatoms[NET_WM_NAME], netatoms[UTF8_STRING], 8,
            PropModeReplace, (unsigned char*)wmname, 12);
    XChangeProperty(dis, root, netatoms[NET_SUPPORTING_WM_CHECK], XA_WINDOW, 32,
            PropModeReplace, (unsigned char*)&supportwin, 1);

    setnumberofdesktops();
    setdesktopnames();
    updatecurrentdesktop();

    /* set the appropriate error handler
     * try an action that will cause an error if another wm is active
     * wait until events are processed to process the error from the above action
     * if all is good set the generic error handler */
    XSetErrorHandler(xerrorstart);
    /* set masks for reporting events handled by the wm */
    XSelectInput(dis, root, ROOTMASK);
    XSync(dis, False);
    XSetErrorHandler(xerror);
    XSync(dis, False);

    grabkeys();
    if (default_desktop < DESKTOPS) change_desktop(&(Arg){.i = default_desktop});
}

void sigchld(__attribute__((unused)) int sig) {
    if (signal(SIGCHLD, sigchld) != SIG_ERR) while (0 < waitpid(-1, NULL, WNOHANG));
    else err(EXIT_FAILURE, "%s: cannot install SIGCHLD handler", wmname);
}

/**
 * execute a command
 */
void spawn(const Arg *arg) {
    if (fork()) return;
    if (dis) close(ConnectionNumber(dis));
    setsid();
    execvp((char*)arg->com[0], (char**)arg->com);
    err(EXIT_SUCCESS, "execvp %s", (char *)arg->com[0]);
}

/**
 * tile or common tiling aka v-stack mode/layout
 * bstack or bottom stack aka h-stack mode/layout
 */
void stack(int x, int y, int w, int h, const Desktop *d) {
    Client *c = NULL, *t = NULL; Bool b = (d->mode == BSTACK);
    int n = 0, clients = 0, p = 0, z = (b ? w:h), ma = (b ? h:w) * d->mfact + d->masz, nm = d->nm;

    /* count stack windows and grab first non-floating, non-fullscreen window */
    for (t = d->head; t; t = t->next) if (!ISFFT(t)) { if (c) ++n; else c = t; }

    /* if there is only one window (c && !n), it should cover the available screen space
     * if there is only one stack window, then we don't care about growth
     * if more than one stack windows (n > 1) adjustments may be needed.
     *
     *   - p is the num of pixels than remain when spliting the
     *       available width/height to the number of windows
     *   - z is each client's height/width
     *
     *      ----------  --.    ----------------------.
     *      |   |----| }--|--> sasz                  }--> first client will have
     *      |   | 1s |    |                          |    z+p+sasz height/width.
     *      | M |----|-.  }--> screen height (h)  ---'
     *      |   | 2s | }--|--> client height (z)    two stack clients on tile mode
     *      -----------' -'                         ::: ascii art by c00kiemon5ter
     *
     * what we do is, remove the sasz from the screen height/width and then
     * divide that space with the windows on the stack so all windows have
     * equal height/width: z = (z - sasz)/n
     *
     * sasz was left out (subtrackted), to later be added to the first client
     * height/width. before we do that, there will be cases when the num of
     * windows cannot be perfectly divided with the available screen height/width.
     * for example: 100px scr. height, and 3 stack windows: 100/3 = 33,3333..
     * so we get that remaining space and merge it to sasz: p = (z - sasz) % n + sasz
     *
     * in the end, we know each client's height/width (z), and how many pixels
     * should be added to the first stack client (p) so that it satisfies sasz,
     * and also, does not result in gaps created on the bottom of the screen.
     */
    if (c && !n) resizeclient(c, x, y, w - 2*c->bw, h - 2*c->bw);
    if (!c || !n) return; else if (n - nm <= 0) nm = n;
    else { p = (z - d->sasz)%((n -= nm - 1)) + d->sasz; z = (z - d->sasz)/n; }

    /* tile non-floating, non-fullscreen master windows to equally share the master area */
    for (int i = 0; i < nm; i++) {
        int xx = (b ? (w - clients) : (h - clients)) / (nm - i);
        if (b) resize(c, x + uselessgap + clients, y + uselessgap, xx - 2*(c->bw + uselessgap),
                ma - 2*(c->bw + uselessgap), False);
        else   resize(c, x + uselessgap, y + uselessgap + clients, ma - 2*(c->bw + uselessgap),
                xx - 2*(c->bw + uselessgap), False);
        clients += (b ? HEIGHT(c) : WIDTH(c)) + uselessgap;
        for (c = c->next; c && ISFFT(c); c = c->next);
    }

    /* tile the next non-floating, non-fullscreen (and first) stack window adding p */
    int ch = z - 2*c->bw - uselessgap; int cw = (b ? h:w) - 2*c->bw - ma - uselessgap;
    if (b) resize(c, x += uselessgap, y += ma, ch - uselessgap + p, cw, False);
    else   resize(c, x += ma, y += uselessgap, cw, ch - uselessgap + p, False);

    /* tile the rest of the non-floating, non-fullscreen stack windows */
    for (b ? (x += z+p-uselessgap):(y += z+p-uselessgap), c = c->next; c; c = c->next) {
        if (ISFFT(c)) continue;
        if (b) { resize(c, x, y, ch, cw, False); x += z; }
        else  { resize(c, x, y, cw, ch, False); y += z; }
    }
}

/**
 * swap master window with current.
 * if current is head swap with next
 * if current is not head, then head
 * is behind us, so move_up until we
 * are the head
 */
void swap_master(void) {
    Desktop *d = &desktops[currdeskidx];
    if (!d->curr || !d->head->next) return;
    if (d->curr == d->head) move_down();
    else while (d->curr != d->head) move_up();
    focus(d->head, d);
}

/**
 * switch tiling mode/layout
 *
 * if mode is reselected reset all floating clients
 * if mode is FLOAT set all clients floating
 */
void switch_mode(const Arg *arg) {
    Desktop *d = &desktops[currdeskidx];
    if (d->mode != arg->i) d->mode = arg->i;
    if (d->head) { tile(d); focus(d->curr, d); }
}

/**
 * tile clients of the given desktop with the desktop's mode/layout
 * call the tiling handler function taking account the panel height
 */
void tile(Desktop *d) {
    if (!d->head || d->mode == FLOAT) return; /* nothing to arange */
    layout[d->head->next ? d->mode:MONOCLE](0, toppanel && d->sbar ? panelheight:0,
            ww, wh + (d->sbar ? 0:panelheight), d);
}

/**
 * toggles the floating state of a client
 */
void togglefloat(void) {
    Desktop *d = &desktops[currdeskidx];
    if (!d->curr || d->curr->isfull || d->curr->isfixed) return;
    if (d->curr->isfloat) d->curr->isfloat = False;
    else d->curr->isfloat = True;
    tile(d);
}

/**
 * toggle visibility state of the panel/bar
 */
void togglepanel(void) {
    desktops[currdeskidx].sbar = !desktops[currdeskidx].sbar;
    tile(&desktops[currdeskidx]);
}

/**
 * windows that request to unmap should lose their client
 * so invisible windows do not exist on screen
 */
void unmapnotify(XEvent *e) {
    Desktop *d = NULL; Client *c = NULL;
    if (wintoclient(e->xunmap.window, &c, &d)) {
        if (e->xunmap.send_event)
            setclientstate(c, WithdrawnState);
        else
            removeclient(c, d);
    }
}

/**
 * sets what desktop a client is on for EWMH aware panels
 */
void updateclientdesktop(Client *c, int desktop) {
    XChangeProperty(dis, c->win, netatoms[NET_WM_DESKTOP], XA_CARDINAL, 32,
            PropModeReplace, (unsigned char *)&(desktop), 1);
}

/**
 * updates the clientlist for EWMH
 */
void updateclientlist(void) {
    Client *c; Desktop *d;
    XDeleteProperty(dis, root, netatoms[NET_CLIENT_LIST]);
    XDeleteProperty(dis, root, netatoms[NET_CLIENT_LIST_STACKING]);
    for (unsigned int i = 0; i < DESKTOPS; i++)
    for (d = &desktops[i], c = d->head; c; c = c->next) {
        XChangeProperty(dis, root, netatoms[NET_CLIENT_LIST], XA_WINDOW, 32,
                PropModeAppend, (unsigned char *)&(c->win), 1);
        XChangeProperty(dis, root, netatoms[NET_CLIENT_LIST_STACKING], XA_WINDOW, 32,
                PropModeAppend, (unsigned char *)&(c->win), 1);
    }
}

/**
 * set the currently focused desktop for EWMH
 */
void updatecurrentdesktop(void) {
	XChangeProperty(dis, root, netatoms[NET_CURRENT_DESKTOP], XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)&(currdeskidx), DESKTOPS);
}

void updatesizehints(Client *c) {
    long msize;
    XSizeHints size;

    if (!XGetWMNormalHints(dis, c->win, &size, &msize))
        /* size is uninitialized, ensure that size.flags aren't used */
        size.flags = PSize;
    if (size.flags & PBaseSize) {
        c->basew = size.base_width;
        c->baseh = size.base_height;
    } else if (size.flags & PMinSize) {
        c->basew = size.min_width;
        c->baseh = size.min_height;
    } else c->basew = c->baseh = 0;
    if (size.flags & PResizeInc) {
        c->incw = size.width_inc;
        c->inch = size.height_inc;
    } else c->incw = c->inch = 0;
    if (size.flags & PMaxSize) {
        c->maxw = size.max_width;
        c->maxh = size.max_height;
    } else c->maxw = c->maxh = 0;
    if (size.flags & PMinSize) {
        c->minw = size.min_width;
        c->minh = size.min_height;
    } else if (size.flags & PBaseSize) {
        c->minw = size.base_width;
        c->minh = size.base_height;
    } else c->minw = c->minh = 0;
    if (size.flags & PAspect) {
        c->mina = (float)size.min_aspect.y / size.min_aspect.x;
        c->maxa = (float)size.max_aspect.x / size.max_aspect.y;
    } else c->maxa = c->mina = 0.0;
    c->isfixed = (c->maxw && c->minw && c->maxh && c->minh
            && c->maxw == c->minw && c->maxh == c->minh);
}

/**
 * find to which client and desktop the given window belongs to
 */
Bool wintoclient(Window w, Client **c, Desktop **d) {
    for (unsigned int i = 0; i < DESKTOPS && !*c; i++)
        for (*d = &desktops[i], *c = (*d)->head; *c && (*c)->win != w; *c = (*c)->next);
    return (*c != NULL);
}

/**
 * There's no way to check accesses to destroyed windows,
 * thus those cases are ignored (especially on UnmapNotify's).
 */
int xerror(__attribute__((unused)) Display *dis, XErrorEvent *ee) {
    if ((ee->error_code == BadAccess   && (ee->request_code == X_GrabKey
                                       ||  ee->request_code == X_GrabButton))
    || (ee->error_code  == BadMatch    && (ee->request_code == X_SetInputFocus
                                       ||  ee->request_code == X_ConfigureWindow))
    || (ee->error_code  == BadDrawable && (ee->request_code == X_PolyFillRectangle
    || ee->request_code == X_CopyArea  ||  ee->request_code == X_PolySegment
                                       ||  ee->request_code == X_PolyText8))
    || ee->error_code   == BadWindow) return 0;
    err(EXIT_FAILURE, "%s: request: %d code: %d", wmname, ee->request_code, ee->error_code);
}

/**
 * error handler function to display an appropriate error message
 * when the window manager initializes (see setup - XSetErrorHandler)
 */
int xerrorstart(__attribute__((unused)) Display *dis, __attribute__((unused)) XErrorEvent *ee) {
    errx(EXIT_FAILURE, "%s: another window manager is already running", wmname);
}

int main(int argc, char *argv[]) {
    if (argc == 2 && !strncmp(argv[1], "-v", 3))
        errx(EXIT_SUCCESS, " %s version: %s - by Unia and c00kiemon5ter", wmname, VERSION);
    else if (argc != 1) errx(EXIT_FAILURE, "usage: man dragonflywm");
    if (!(dis = XOpenDisplay(NULL))) errx(EXIT_FAILURE, "%s: cannot open display", wmname);
    setup();
    run();
    cleanup();
    XCloseDisplay(dis);
    return retval;
}

/* vim: set expandtab ts=4 sts=4 sw=4 : */
