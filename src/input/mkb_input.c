/*
 * mkb_input.c -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * Two front ends feed the same accumulators:
 *
 *  - GDI build (Windows, XBOX_SDL_WINDOW off): a hidden window on its own
 *    thread registers for Raw Input with RIDEV_INPUTSINK, so mouse deltas
 *    and key presses arrive whether or not the game's own window is the one
 *    with focus, and no matter which window the runtime happens to present
 *    through (the GDI debug window, the FMV window, a D3D11 swap chain).
 *
 *  - SDL build (POSIX, and Windows with XBOX_SDL_WINDOW): the SDL window's
 *    thread (src/video/fb_present_sdl.c) hands every event to
 *    mkb_sdl_event() and calls mkb_sdl_tick() once per loop. Capture is
 *    SDL's relative mouse mode, on only while the window has the keyboard
 *    focus, F9 has not released it and no overlay (the settings menu) owns
 *    the input. There is no raw-input thread here: on Windows SDL's relative
 *    mode registers the process's raw mouse itself, and a process has one
 *    raw-input target per device class.
 *
 * The poll side drains the accumulated deltas and turns them into a
 * stick deflection that is proportional to the movement since the last
 * poll: the title integrates deflection over its own frame, so the
 * total rotation comes out proportional to total mouse travel at any
 * frame rate, which is the property mouse aim has to keep.
 *
 * Small movements get a floor of RECOMP_MKB_DZ of full throw, because
 * every console title applies its own dead zone to the stick, and a
 * mouse correction smaller than it would be silently eaten.
 */

#if defined(_WIN32) && !defined(XBOX_SDL_WINDOW)
#define MKB_RAWINPUT 1
#else
#define MKB_RAWINPUT 0
#endif

#if defined(_WIN32)
/* Raw input needs a modern _WIN32_WINNT; the toolkit's platform header
 * includes windows.h first, so these have to land before anything else. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "mkb_input.h"

#if defined(_WIN32)
#include <windows.h>
#endif
#if !MKB_RAWINPUT
#include <SDL.h>
#include "keyboard_sdl.h"     /* xbox_InputSdlKeyToVk: the one keycode map */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- atomics: Interlocked* on the raw-input build, SDL's elsewhere ------ */

#if MKB_RAWINPUT
typedef volatile LONG mkb_atom;
#define MKB_GET(a)        InterlockedCompareExchange((a), 0, 0)
#define MKB_SET(a, v)     InterlockedExchange((a), (v))
#define MKB_TAKE(a)       InterlockedExchange((a), 0)
#define MKB_ADD(a, v)     InterlockedAdd((a), (v))
#define MKB_OR(a, v)      InterlockedOr((a), (v))
#define MKB_AND(a, v)     InterlockedAnd((a), (v))
#define MKB_CAS(a, o, n)  (InterlockedCompareExchange((a), (n), (o)) == (o))
#define MKB_YIELD()       Sleep(1)
#else
typedef SDL_atomic_t mkb_atom;
#define MKB_GET(a)        SDL_AtomicGet(a)
#define MKB_SET(a, v)     SDL_AtomicSet((a), (v))
#define MKB_TAKE(a)       SDL_AtomicSet((a), 0)
#define MKB_ADD(a, v)     SDL_AtomicAdd((a), (v))
#define MKB_CAS(a, o, n)  SDL_AtomicCAS((a), (o), (n))
#define MKB_YIELD()       SDL_Delay(1)
static void MKB_OR(SDL_atomic_t *a, int v)
{
    int o;
    do o = SDL_AtomicGet(a); while (!SDL_AtomicCAS(a, o, o | v));
}
static void MKB_AND(SDL_atomic_t *a, int v)
{
    int o;
    do o = SDL_AtomicGet(a); while (!SDL_AtomicCAS(a, o, o & v));
}
#endif

/* Windows VK codes, spelled out so the POSIX build needs no windows.h. */
#define MKB_VK_TAB      0x09
#define MKB_VK_RETURN   0x0D
#define MKB_VK_SHIFT    0x10
#define MKB_VK_CONTROL  0x11
#define MKB_VK_ESCAPE   0x1B
#define MKB_VK_SPACE    0x20
#define MKB_VK_LEFT     0x25
#define MKB_VK_UP       0x26
#define MKB_VK_RIGHT    0x27
#define MKB_VK_DOWN     0x28
#define MKB_VK_F9       0x78
#define MKB_VK_LSHIFT   0xA0
#define MKB_VK_LCONTROL 0xA2

#define MKB_PATH_MAX    1024

/* ---- configuration ----------------------------------------------------- */

typedef struct {
    double sens;        /* multiplier on the aim deflection          */
    double dz;          /* assumed game deadzone, 0..1               */
    double acc;         /* aim acceleration exponent, 1 = linear     */
    long   full;        /* mouse counts for full throw, per poll     */
    int    invert_y;    /* invert the vertical aim axis              */
    int    swap_sticks; /* mouse aims with the left stick instead    */
} MkbConfig;

static MkbConfig s_cfg = { 1.0, 0.18, 1.0, 8, 0, 0 };

/* One "key=value" line; returns 1 when the key was recognised. */
static int mkb_apply_kv(const char *key, const char *val)
{
    double d;
    long   l;

    if (!strcmp(key, "sens")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 100.0) s_cfg.sens = d;
    } else if (!strcmp(key, "dz")) {
        d = strtod(val, NULL); if (d >= 0.0 && d < 0.9) s_cfg.dz = d;
    } else if (!strcmp(key, "acc")) {
        d = strtod(val, NULL); if (d > 0.0 && d < 10.0) s_cfg.acc = d;
    } else if (!strcmp(key, "full")) {
        l = strtol(val, NULL, 0); if (l > 0 && l < 1000) s_cfg.full = l;
    } else if (!strcmp(key, "invert_y")) {
        s_cfg.invert_y = atoi(val) != 0;
    } else if (!strcmp(key, "swap_sticks")) {
        s_cfg.swap_sticks = atoi(val) != 0;
    } else {
        return 0;
    }
    return 1;
}

/* mkb.ini beside the executable, unless RECOMP_MKB_INI names one. */
static int mkb_ini_path(char *path, size_t cap)
{
    const char *ini = getenv("RECOMP_MKB_INI");

    if (ini && *ini) {
        snprintf(path, cap, "%s", ini);
        return 1;
    }
#if MKB_RAWINPUT
    {
        DWORD n = GetModuleFileNameA(NULL, path, (DWORD)(cap - 12));
        if (!n || n >= cap - 12)
            return 0;
        snprintf(path + n, 12, "\\..\\mkb.ini");   /* beside the .exe */
    }
#else
    {
        char *base = SDL_GetBasePath();             /* ends in a separator */
        if (!base)
            return 0;
        snprintf(path, cap, "%smkb.ini", base);
        SDL_free(base);
    }
#endif
    return 1;
}

static void mkb_load_config(void)
{
    static const char *kv[] = {
        "RECOMP_MKB_SENS", "sens",
        "RECOMP_MKB_DZ",   "dz",
        "RECOMP_MKB_ACC",  "acc",
        "RECOMP_MKB_FULL", "full",
        "RECOMP_MKB_INVERT_Y",  "invert_y",
        "RECOMP_MKB_SWAP_STICKS", "swap_sticks",
        NULL
    };
    char path[MKB_PATH_MAX];
    FILE *f;
    int i;

    for (i = 0; kv[i]; i += 2) {
        const char *v = getenv(kv[i]);
        if (v && *v)
            mkb_apply_kv(kv[i + 1], v);
    }

    if (!mkb_ini_path(path, sizeof path))
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            char *k, *v;
            if (!eq) continue;
            *eq = 0;
            k = line; v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            while (*v == ' ' || *v == '\t') v++;
            k[strcspn(k, " \t\r\n")] = 0;
            v[strcspn(v, " \t\r\n")] = 0;
            if (*k && *v)
                mkb_apply_kv(k, v);
        }
    }
    fclose(f);
}

/* ---- shared state ------------------------------------------------------ */

/* Accumulators, written by the input side (the raw-input thread, or the SDL
 * window's thread) and drained by the poll. Add on the way in, take
 * everything with an exchange to 0 on the way out. */
static mkb_atom s_dx, s_dy;
static mkb_atom s_wheel;           /* wheel steps since last poll       */
/* Mouse buttons, 1 L, 2 R, 4 M, 8 X1, 16 X2: what is held now, and what
 * went down since the last poll (so a click shorter than a poll counts). */
static mkb_atom s_mouse_btn;
static mkb_atom s_mouse_click;
/* One byte per VK, written by the input side, read by the poll; a press
 * seen a poll late is one made a poll later (keyboard.c's rule). */
static volatile unsigned char s_vk_down[256];

static mkb_atom s_active = { 1 };  /* F9 toggles this                   */
static mkb_atom s_overlay;         /* an overlay owns the input         */
static mkb_atom s_init;            /* 0 not yet, 1 going, 2 off, 3 on   */

static void mkb_toggle(void)
{
    int now = !MKB_GET(&s_active);
    MKB_SET(&s_active, now);
    fprintf(stderr, "  [MKB] %s\n", now ? "captured" : "released (F9)");
    fflush(stderr);
}

#if MKB_RAWINPUT
/* ---- raw input thread (GDI build) -------------------------------------- */

static mkb_atom s_thread_up;

static void mkb_btn(USHORT flags, USHORT down, USHORT up, LONG bit)
{
    if (flags & down) {
        MKB_OR(&s_mouse_btn, bit);
        MKB_OR(&s_mouse_click, bit);
    }
    if (flags & up)
        MKB_AND(&s_mouse_btn, ~bit);
}

static void mkb_handle_raw(HRAWINPUT h)
{
    BYTE buf[sizeof(RAWINPUT) + 64];
    UINT size = sizeof buf;
    RAWINPUT *ri = (RAWINPUT *)buf;

    if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER))
            == (UINT)-1)
        return;

    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE *m = &ri->data.mouse;
        USHORT f = m->usButtonFlags;

        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) {
            if (m->lLastX) MKB_ADD(&s_dx, m->lLastX);
            if (m->lLastY) MKB_ADD(&s_dy, m->lLastY);
        }
        mkb_btn(f, RI_MOUSE_LEFT_BUTTON_DOWN,   RI_MOUSE_LEFT_BUTTON_UP,   1);
        mkb_btn(f, RI_MOUSE_RIGHT_BUTTON_DOWN,  RI_MOUSE_RIGHT_BUTTON_UP,  2);
        mkb_btn(f, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 4);
        mkb_btn(f, RI_MOUSE_BUTTON_4_DOWN,      RI_MOUSE_BUTTON_4_UP,      8);
        mkb_btn(f, RI_MOUSE_BUTTON_5_DOWN,      RI_MOUSE_BUTTON_5_UP,      16);
        if (f & RI_MOUSE_WHEEL) {
            SHORT d = (SHORT)m->usButtonData;
            MKB_ADD(&s_wheel, d / WHEEL_DELTA);
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri->data.keyboard;
        UINT vk = k->VKey;

        /* F9 is read on the input thread, so the toggle works even while
         * the game is stalled on the poll side. */
        if (vk == MKB_VK_F9 && k->Message == WM_KEYDOWN && !s_vk_down[vk])
            mkb_toggle();
        if (vk && vk < 256) {
            if (k->Message == WM_KEYDOWN || k->Message == WM_SYSKEYDOWN)
                s_vk_down[vk] = 1;
            else
                s_vk_down[vk] = 0;
        }
    }
}

static LRESULT CALLBACK mkb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_INPUT) {
        mkb_handle_raw((HRAWINPUT)l);
        return 0;
    }
    if (m == WM_CLOSE || m == WM_DESTROY)
        return 0;
    return DefWindowProcA(h, m, w, l);
}

static DWORD WINAPI mkb_thread(LPVOID unused)
{
    WNDCLASSA wc;
    HWND hwnd;
    RAWINPUTDEVICE rid[2];
    MSG msg;

    (void)unused;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = mkb_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "XboxRecompMKB";
    RegisterClassA(&wc);
    /* Not visible: it exists to own the WM_INPUT delivery, nothing else. */
    hwnd = CreateWindowExA(0, "XboxRecompMKB", "mkb", WS_OVERLAPPED,
                           0, 0, 0, 0, NULL, NULL,
                           GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        fprintf(stderr, "  [MKB] window creation failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    memset(rid, 0, sizeof rid);
    rid[0].usUsagePage = 0x01;  rid[0].usUsage = 0x02;  /* mouse     */
    rid[1].usUsagePage = 0x01;  rid[1].usUsage = 0x06;  /* keyboard  */
    rid[0].dwFlags = RIDEV_INPUTSINK;   /* deliver without focus */
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = hwnd;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        fprintf(stderr, "  [MKB] RegisterRawInputDevices failed (%lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }

    MKB_SET(&s_thread_up, 1);
    fprintf(stderr, "  [MKB] mouse+keyboard captured (F9 releases; "
            "sens=%.2f dz=%.2f full=%ld)\n",
            s_cfg.sens, s_cfg.dz, s_cfg.full);
    fflush(stderr);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    MKB_SET(&s_thread_up, 0);
    return 0;
}

/* The recompiled game's process is the foreground one. Raw input with
 * INPUTSINK keeps arriving while the user is in another app, and the
 * mouse must go on behaving normally there. */
static int game_is_foreground(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* Hide and confine the pointer while captured. Aiming with raw deltas
 * does not move the pointer by itself, but Windows still moves it from
 * the same physical movement, so it would wander across the screen and
 * end up clicking other apps' title bars. */
static void mkb_manage_cursor(int capture)
{
    static int hidden;          /* how many ShowCursor(FALSE) we owe  */
    static unsigned long last;

    unsigned long now = GetTickCount();
    if (now - last < 500 && (capture ? hidden : !hidden) == 0)
        return;
    last = now;

    if (capture) {
        RECT r;
        HWND fg = GetForegroundWindow();
        if (fg && GetWindowRect(fg, &r))
            ClipCursor(&r);
        if (hidden < 5) { ShowCursor(FALSE); hidden++; }
    } else {
        if (hidden > 0) { ShowCursor(TRUE); hidden--; }
        ClipCursor(NULL);
    }
}

#else /* !MKB_RAWINPUT */
/* ---- SDL window events (SDL build) ------------------------------------- */

/* Window thread only: relative mode is on (we turned it on). */
static int      s_rel;
static int      s_rel_failed;
/* The same, for the poll: deltas only count while captured. */
static mkb_atom s_captured;

/* The keys the pad reads, as Windows VKs: the shared map (letters, Return,
 * Shift, Ctrl, arrows) plus the ones it has no row for. */
static int mkb_sdl_vk(SDL_Keycode k)
{
    switch (k) {
    case SDLK_SPACE:  return MKB_VK_SPACE;
    case SDLK_TAB:    return MKB_VK_TAB;
    case SDLK_ESCAPE: return MKB_VK_ESCAPE;
    case SDLK_F9:     return MKB_VK_F9;
    }
    return xbox_InputSdlKeyToVk((int)k);
}

static int mkb_sdl_bit(Uint8 button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:   return 1;
    case SDL_BUTTON_RIGHT:  return 2;
    case SDL_BUTTON_MIDDLE: return 4;
    case SDL_BUTTON_X1:     return 8;
    case SDL_BUTTON_X2:     return 16;
    }
    return 0;
}

static void mkb_release_all(void)
{
    MKB_TAKE(&s_dx);
    MKB_TAKE(&s_dy);
    MKB_TAKE(&s_wheel);
    MKB_TAKE(&s_mouse_btn);
    MKB_TAKE(&s_mouse_click);
}

int mkb_sdl_event(const SDL_Event *ev)
{
    int vk, bit;

    if (!mkb_enabled())
        return 0;
    switch (ev->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        vk = mkb_sdl_vk(ev->key.keysym.sym);
        if (vk == MKB_VK_F9) {
            if (ev->type == SDL_KEYDOWN && !ev->key.repeat)
                mkb_toggle();
            return 1;                   /* ours; no one else maps F9 */
        }
        if (vk > 0 && vk < 256 && !ev->key.repeat)
            s_vk_down[vk] = ev->type == SDL_KEYDOWN;
        return 0;                       /* the keyboard pad reads keys too */
    case SDL_MOUSEMOTION:
        if (!s_rel)
            return 0;                   /* a free pointer is not aim */
        if (ev->motion.xrel) MKB_ADD(&s_dx, ev->motion.xrel);
        if (ev->motion.yrel) MKB_ADD(&s_dy, ev->motion.yrel);
        return 1;
    case SDL_MOUSEBUTTONDOWN:
        bit = mkb_sdl_bit(ev->button.button);
        if (!s_rel || !bit)
            return 0;   /* the click that focuses the window is not fire */
        MKB_OR(&s_mouse_btn, bit);
        MKB_OR(&s_mouse_click, bit);
        return 1;
    case SDL_MOUSEBUTTONUP:
        bit = mkb_sdl_bit(ev->button.button);
        if (bit)
            MKB_AND(&s_mouse_btn, ~bit);  /* always: never stuck held */
        return s_rel;
    case SDL_MOUSEWHEEL:
        if (!s_rel)
            return 0;
        {
            int y = ev->wheel.y;
            if (ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                y = -y;
            if (y)
                MKB_ADD(&s_wheel, y);
        }
        return 1;
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST
                || ev->window.event == SDL_WINDOWEVENT_HIDDEN
                || ev->window.event == SDL_WINDOWEVENT_MINIMIZED) {
            memset((void *)s_vk_down, 0, sizeof s_vk_down);
            mkb_release_all();
        }
        return 0;
    }
    return 0;
}

/* Once per window-loop pass, after its events: relative mode on exactly
 * while the pad should own the mouse. SDL itself drops the grab on focus
 * loss and takes it back on focus gain; turning relative mode off here as
 * well keeps the pointer free for an overlay and makes the refocusing
 * click land before capture, where it is ignored. */
void mkb_sdl_tick(void)
{
    SDL_Window *focus;
    int want;

    if (!mkb_enabled())
        return;
    focus = SDL_GetKeyboardFocus();
    want = MKB_GET(&s_active) && !MKB_GET(&s_overlay) && focus
        && !(SDL_GetWindowFlags(focus) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));
    if (want == s_rel || (want && s_rel_failed))
        return;
    if (want && SDL_SetRelativeMouseMode(SDL_TRUE) != 0) {
        fprintf(stderr, "  [MKB] relative mouse mode unavailable (%s)\n",
                SDL_GetError());
        fflush(stderr);
        s_rel_failed = 1;
        return;
    }
    if (!want) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        s_rel_failed = 0;
    }
    s_rel = want;
    MKB_SET(&s_captured, want);
    mkb_release_all();
    {
        static int said;
        if (want && !said++)
            fprintf(stderr, "  [MKB] mouse+keyboard captured (F9 releases; "
                    "sens=%.2f dz=%.2f full=%ld)\n",
                    s_cfg.sens, s_cfg.dz, s_cfg.full);
        else if (getenv("RECOMP_MKB_TRACE"))
            fprintf(stderr, "  [MKB] relative mode %s\n", want ? "on" : "off");
        fflush(stderr);
    }
}

#endif /* MKB_RAWINPUT */

/* ---- poll side --------------------------------------------------------- */

int mkb_enabled(void)
{
    int st = MKB_GET(&s_init);

    if (st >= 2)
        return st == 3;
    /* Once, on whichever thread gets here first (the poll, or the SDL
     * window's thread); the others wait for the answer. */
    if (MKB_CAS(&s_init, 0, 1)) {
        /* Opt-in, like RECOMP_KEYBOARD: a keyboard silently acting as
         * player 1 is surprising when a real pad is what you meant to use.
         * RECOMP_MKB=1 turns the layer on. */
        const char *v = getenv("RECOMP_MKB");
        int on = v && *v && *v != '0';
        if (on) {
            mkb_load_config();
#if MKB_RAWINPUT
            CloseHandle(CreateThread(NULL, 0, mkb_thread, NULL, 0, NULL));
#endif
        }
        MKB_SET(&s_init, on ? 3 : 2);
        return on;
    }
    while ((st = MKB_GET(&s_init)) == 1)
        MKB_YIELD();
    return st == 3;
}

void mkb_set_overlay(int open)
{
    MKB_SET(&s_overlay, open ? 1 : 0);
}

/* Radial deadzone compensation: an input radius in [0..1] comes out as
 * [dz..1], so the smallest mouse correction still clears the title's
 * own deadzone, and the response stays linear above it. */
static void mkb_aim_vector(double ax, double ay, SHORT *out_x, SHORT *out_y)
{
    double r = sqrt(ax * ax + ay * ay);
    double len, fx, fy;
    long v;

    if (r <= 0.0) { *out_x = 0; *out_y = 0; return; }
    if (r > 1.0) r = 1.0;
    len = s_cfg.dz + (1.0 - s_cfg.dz) * pow(r, s_cfg.acc);
    len *= s_cfg.sens;
    if (len > 1.0) len = 1.0;
    fx = ax * len / sqrt(ax * ax + ay * ay);
    fy = ay * len / sqrt(ax * ax + ay * ay);
    v = (long)(fx * 32767.0);
    *out_x = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = (long)(fy * 32767.0);
    *out_y = (SHORT)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

#define MK_VK(k)  ((unsigned)(k) < 256 && s_vk_down[(k)])

int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    SHORT aim_x = 0, aim_y = 0, mov_x = 0, mov_y = 0;
    WORD b = 0;
    BYTE analog[8];
    LONG dx, dy, wheel, btn;
    int any = 0, i;
    static DWORD packet;

    if (!pState || dwPort != 0 || !mkb_enabled())
        return 0;

#if MKB_RAWINPUT
    if (!MKB_GET(&s_thread_up))
        return 0;

    if (!MKB_GET(&s_active) || MKB_GET(&s_overlay)) {
        mkb_manage_cursor(0);
        return 0;
    }

    /* Foreground gate: while the user is elsewhere the mouse belongs to
     * whoever has it, and the game should read a pad at rest. */
    if (!game_is_foreground()) {
        mkb_manage_cursor(0);
        MKB_TAKE(&s_dx);
        MKB_TAKE(&s_dy);
        return 0;
    }
    mkb_manage_cursor(1);
#else
    /* Captured = the window thread has relative mode on: focused, F9 not
     * released, no overlay. Anything else is a pad at rest. */
    if (!MKB_GET(&s_captured)) {
        MKB_TAKE(&s_dx);
        MKB_TAKE(&s_dy);
        return 0;
    }
#endif

    dx    = MKB_TAKE(&s_dx);
    dy    = MKB_TAKE(&s_dy);
    wheel = MKB_TAKE(&s_wheel);
    btn   = MKB_GET(&s_mouse_btn) | MKB_TAKE(&s_mouse_click);
#if !MKB_RAWINPUT
    /* RECOMP_MKB_TRACE: what the polls drained, summed, at most 4 lines/s. */
    if (getenv("RECOMP_MKB_TRACE")) {
        static LONG sum_dx, sum_dy, sum_wheel, polls, btn_polls;
        static Uint32 last;
        Uint32 now = SDL_GetTicks();
        sum_dx += dx;
        sum_dy += dy;
        sum_wheel += wheel;
        polls++;
        if (btn)
            btn_polls++;
        if (now - last >= 250) {
            if (sum_dx || sum_dy || sum_wheel || btn_polls) {
                fprintf(stderr, "  [MKB] polls %ld: dx %ld dy %ld wheel %ld, "
                        "buttons 0x%lx held in %ld\n", polls, sum_dx, sum_dy,
                        sum_wheel, btn, btn_polls);
                fflush(stderr);
            }
            sum_dx = sum_dy = sum_wheel = polls = btn_polls = 0;
            last = now;
        }
    }
#endif

    memset(analog, 0, sizeof analog);

    /* ---- mouse buttons, as the Xbox pad sees them ---- */
    if (btn & 0x01) analog[XBOX_BUTTON_RTRIGGER] = 255;  /* fire      */
    if (btn & 0x02) analog[XBOX_BUTTON_LTRIGGER] = 255;  /* ADS/aim   */

    /* ---- keyboard: WASD movement ---- */
    {
        int mx = 0, my = 0;
        if (MK_VK('W')) my += 1;
        if (MK_VK('S')) my -= 1;
        if (MK_VK('A')) mx -= 1;
        if (MK_VK('D')) mx += 1;
        if (mx && my) { mx = (int)(mx * 70.71); my = (int)(my * 70.71); }
        if (mx || my) mkb_aim_vector(mx / 100.0, my / 100.0, &mov_x, &mov_y);
    }

    /* ---- mouse: aim ---- */
    if (dx || dy) {
        double ax =  (double)dx / (double)s_cfg.full;
        double ay = -(double)dy / (double)s_cfg.full;
        if (s_cfg.invert_y) ay = -ay;
        mkb_aim_vector(ax, ay, &aim_x, &aim_y);
    }

    /* ---- keyboard: buttons (Black's layout) ---- */
    if (MK_VK(MKB_VK_SPACE)) analog[XBOX_BUTTON_A]     = 255; /* use/jump */
    if (MK_VK('R'))          analog[XBOX_BUTTON_X]     = 255; /* reload    */
    if (MK_VK('E'))          analog[XBOX_BUTTON_B]     = 255; /* alt use   */
    if (MK_VK('Q'))          analog[XBOX_BUTTON_WHITE] = 255; /* grenade   */
    if (MK_VK('F'))          analog[XBOX_BUTTON_BLACK] = 255; /* alt gren. */
    if (btn & 0x08)          analog[XBOX_BUTTON_WHITE] = 255; /* X1        */
    if (btn & 0x10)          analog[XBOX_BUTTON_BLACK] = 255; /* X2        */
    if (MK_VK(MKB_VK_RETURN) || MK_VK(MKB_VK_ESCAPE)) b |= XBOX_GAMEPAD_START;
    if (MK_VK(MKB_VK_TAB))                            b |= XBOX_GAMEPAD_BACK;
    /* Raw input and SDL both report either Shift as VK_SHIFT (and Ctrl as
     * VK_CONTROL); VK_LSHIFT / VK_LCONTROL kept in case a host sends them. */
    if (MK_VK(MKB_VK_SHIFT) || MK_VK(MKB_VK_LSHIFT))
        b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (MK_VK(MKB_VK_CONTROL) || MK_VK(MKB_VK_LCONTROL))
        b |= XBOX_GAMEPAD_RIGHT_THUMB;
    if (MK_VK(MKB_VK_UP))    b |= XBOX_GAMEPAD_DPAD_UP;
    if (MK_VK(MKB_VK_DOWN))  b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (MK_VK(MKB_VK_LEFT))  b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (MK_VK(MKB_VK_RIGHT)) b |= XBOX_GAMEPAD_DPAD_RIGHT;

    /* ---- wheel / middle button: cycle weapon ---- */
    if (wheel || (btn & 0x04))
        analog[XBOX_BUTTON_Y] = 255;

    /* ---- merge on top of whatever the host pad produced ---- */
    if (b)        { pState->Gamepad.wButtons |= b; any = 1; }
    for (i = 0; i < 8; i++) {
        if (analog[i]) {
            if (analog[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = analog[i];
            any = 1;
        }
    }
    if (aim_x || aim_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbLX = aim_x;
            pState->Gamepad.sThumbLY = aim_y;
        } else {
            pState->Gamepad.sThumbRX = aim_x;
            pState->Gamepad.sThumbRY = aim_y;
        }
        any = 1;
    }
    if (mov_x || mov_y) {
        if (s_cfg.swap_sticks) {
            pState->Gamepad.sThumbRX = mov_x;
            pState->Gamepad.sThumbRY = mov_y;
        } else {
            pState->Gamepad.sThumbLX = mov_x;
            pState->Gamepad.sThumbLY = mov_y;
        }
        any = 1;
    }

    /* Edge detection: a title polling for changes ignores a state whose
     * packet number did not move. */
    if (any)
        pState->dwPacketNumber = ++packet;
    return any;
}
