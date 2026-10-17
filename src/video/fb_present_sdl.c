/**
 * Show the guest framebuffer in an SDL2 window: the POSIX half of
 * fb_present.c, and on Windows its replacement when built with
 * XBOX_SDL_WINDOW (one window layer for every host).
 *
 * fb_present.c is Win32 GDI and runs its window on a thread of its own. That
 * cannot work on macOS, where AppKit -- and so SDL's video and event pump --
 * only runs on the process main thread, and the guest would otherwise own
 * that thread for the whole run. So on POSIX the shape is inverted here:
 *
 *   process main thread   SDL window, the one SDL event loop, the presents
 *   "guest-main" pthread  the boot sequence and the recompiled entry point
 *
 * A title opts in by handing its boot sequence to xbox_HostWindowMain()
 * from main(). With RECOMP_FB_WINDOW set (the same switch as the Win32
 * window) it opens the window, starts the guest on a pthread with at least
 * the main thread's stack and, on macOS, its QoS, and pumps events and
 * presents until the guest returns or the window is closed. Without it, or
 * with RECOMP_HEADLESS=1, no display, or SDL video failing, the guest runs
 * on the calling thread exactly as before.
 *
 * On Windows a window may live on any thread, so the GDI window's shape is
 * kept: xbox_FramebufferWindowStart() (called at the first AvSetDisplayMode
 * / surface setup) opens it on an "fb window" thread that owns the window
 * and its event loop, and the guest stays where it is.
 *
 * Frames come from the executor's flip (xbox_FramebufferWindowPresent, on
 * the executor's thread). The flip copies the finished surface into a free
 * slot of three and publishes it with an index swap under a lock held for a
 * few instructions, so the executor never waits for the window and the
 * window never reads guest memory the rasteriser may be writing. The surface
 * format is the GDI window's: 640x480, bpp from pitch / 640, 2 = R5G6B5,
 * 4 = X8R8G8B8. Before the first flip the window reads the AvSetDisplayMode
 * surface directly, as the GDI window does, for a title that never flips.
 *
 * A renderer that draws into the window itself (a GL context on its HWND)
 * calls xbox_FramebufferWindowExternalPresent(1) before the window opens:
 * the window then gets no SDL_Renderer (whose swap chain would fight the
 * GL context for it) and shows nothing of its own; Hwnd / ClientSize give
 * that renderer what it needs, as the GDI window does.
 *
 * Closing the window ends the process: lifted code cannot be stopped
 * mid-flight. On POSIX atexit handlers still run; on Windows the process is
 * terminated, as the GDI window does (a guest thread killed holding a CRT
 * stream lock would hang the exit flush).
 *
 * Alt+Enter toggles borderless fullscreen (the game does not see it as
 * Start); RECOMP_FB_FULLSCREEN=1 starts fullscreen.
 *
 * Knobs: RECOMP_PRESENT_VSYNC=0 presents without waiting for the display;
 * RECOMP_WINDOW_SCALE=<n> sizes the window (default 2 on POSIX; 1 on
 * Windows, where the size also follows the monitor's DPI, as the GDI
 * window's).
 */
#if !defined(_WIN32) || defined(XBOX_SDL_WINDOW)

#define SDL_MAIN_HANDLED      /* the title has its own main / WinMain */
#include <SDL.h>
#include <SDL_syswm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include "../../include/xbox/thread_name.h"
#else
#include <pthread.h>
#include <sys/resource.h>
#endif

#include "video_player.h"
#include "keyboard_sdl.h"     /* src/input: keys into the pad stand-in */

extern ptrdiff_t xbox_GetMemoryOffset(void);
#if !defined(_WIN32)
extern void xbox_InputSetMainPump(int on);   /* src/input/xinput_device.c */
extern int  xbox_InputAnyOpen(void);
#endif

#define FB_W 640u
#define FB_H 480u

/* ── Handoff: three slots, one each for writer, reader and the ready frame ── */

struct fb_slot {
    uint8_t *px;
    size_t   cap;
    uint32_t bpp;
};

/* What the window takes from SDL, and gives back on every way out. On
 * Windows the pads are still XInput's (src/input), so no game controllers. */
#if defined(_WIN32)
#define FBWIN_SDL_SUBSYSTEMS (SDL_INIT_VIDEO | SDL_INIT_EVENTS)
#else
#define FBWIN_SDL_SUBSYSTEMS \
    (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)
#endif

static struct fb_slot  s_slot[3];
static int             s_back = 0, s_ready = 1, s_front = 2;
static uint64_t        s_ready_seq;          /* frames published */
static SDL_SpinLock    s_lock;
static SDL_atomic_t    s_running;            /* window up: flips copy */
static Uint32          s_frame_event;        /* wakes the main loop */
static SDL_atomic_t    s_wake_pending;
static SDL_atomic_t    s_in_present;         /* executor inside Present */
static volatile uint32_t s_fb_va, s_fb_pitch; /* AvSetDisplayMode surface */

/* External present (a renderer drawing into the window itself). */
static int             s_external;           /* set before the window opens */
static void *volatile  s_hwnd;               /* NULL until the window exists */
static SDL_atomic_t    s_client_wh;          /* w << 16 | h, physical px; 0 = none */

/* Counted at the flip, read once a second for the title bar. */
static SDL_atomic_t    s_flips;
static SDL_atomic_t    s_frame_draws;

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    s_fb_va = fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* ponytail: the SDL window shows the surface unscaled; 2x2 anti-aliased
 * surfaces (#162) come through at twice the height until it honours this. */
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }

/* The back slot is filled: make it the ready one. An unshown ready frame is
 * overwritten (dropped), never waited for. */
static void publish(uint32_t bpp)
{
    s_slot[s_back].bpp = bpp;          /* only this thread touches s_back */
    SDL_AtomicLock(&s_lock);
    {
        int t = s_ready;
        s_ready = s_back;
        s_back = t;
        s_ready_seq++;
    }
    SDL_AtomicUnlock(&s_lock);

    /* Wake the main loop, once per batch of frames it has not looked at. */
    if (SDL_AtomicSet(&s_wake_pending, 1) == 0) {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_frame_event;
        /* Not queued (full, or filtered): clear the latch, or no frame
         * would wake the loop again before its timeout. */
        if (SDL_PushEvent(&ev) <= 0)
            SDL_AtomicSet(&s_wake_pending, 0);
    }
}

static void present_copy(uint32_t fb_va, uint32_t pitch)
{
    struct fb_slot *s = &s_slot[s_back];
    const uint8_t *src;
    uint32_t bpp = pitch / FB_W, y;
    size_t need;

    if (bpp != 2 && bpp != 4)
        return;
    need = (size_t)FB_W * FB_H * bpp;
    if (s->cap < need) {
        uint8_t *n = (uint8_t *)realloc(s->px, need);
        if (!n)
            return;
        s->px = n;
        s->cap = need;
    }
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < FB_H; y++)
        memcpy(s->px + (size_t)y * FB_W * bpp, src + (size_t)y * pitch,
               (size_t)FB_W * bpp);
    publish(bpp);
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    /* Dekker-style handshake with the close path: each side stores its own
     * flag and then loads the other's (SDL's atomics are full barriers), so
     * either the window sees this present in flight or this present sees
     * the window closing. */
    if (!SDL_AtomicGet(&s_running) || s_external || !fb_va || !pitch)
        return;
    SDL_AtomicAdd(&s_in_present, 1);
    if (SDL_AtomicGet(&s_running))
        present_copy(fb_va, pitch);
    SDL_AtomicAdd(&s_in_present, -1);
}

/* A finished frame from an external renderer (nv2a_pb_hook.h): 0x00RRGGBB
 * rows, top-down, cropped or padded with black to the window's size. */
static void present_pixels_copy(const uint32_t *xrgb, uint32_t width,
                                uint32_t height)
{
    struct fb_slot *s = &s_slot[s_back];
    size_t need = (size_t)FB_W * FB_H * 4;
    uint32_t w = width < FB_W ? width : FB_W;
    uint32_t h = height < FB_H ? height : FB_H, y;

    if (s->cap < need) {
        uint8_t *n = (uint8_t *)realloc(s->px, need);
        if (!n)
            return;
        s->px = n;
        s->cap = need;
    }
    if (w < FB_W || h < FB_H)
        memset(s->px, 0, need);
    for (y = 0; y < h; y++)
        memcpy(s->px + (size_t)y * FB_W * 4, xrgb + (size_t)y * width,
               (size_t)w * 4);
    publish(4);
}

void xbox_FramebufferWindowPresentPixels(const uint32_t *xrgb, uint32_t width,
                                         uint32_t height)
{
    /* With an external present the window has no renderer to show these
     * (the frames before that renderer is up); they are dropped. */
    if (!SDL_AtomicGet(&s_running) || s_external || !xrgb)
        return;
    SDL_AtomicAdd(&s_in_present, 1);
    if (SDL_AtomicGet(&s_running))
        present_pixels_copy(xrgb, width, height);
    SDL_AtomicAdd(&s_in_present, -1);
}

void xbox_FramebufferWindowExternalPresent(int on)
{
    s_external = on ? 1 : 0;
}

void *xbox_FramebufferWindowHwnd(void) { return s_hwnd; }

/* Nothing to stop: in external mode the window draws nothing of its own. */
void xbox_FramebufferWindowGpuPresent(int on) { (void)on; }

int xbox_FramebufferWindowClientSize(uint32_t *width, uint32_t *height)
{
    uint32_t wh = (uint32_t)SDL_AtomicGet(&s_client_wh);

    if (width)
        *width = (wh >> 16) & 0xFFFF;
    if (height)
        *height = wh & 0xFFFF;
    return wh != 0;
}

/* Title bar: "<XBE title> | FPS: n | draws: n", as the GDI window shows it.
 * Written by the loader and the flip, read once a second by the main
 * thread; a torn read shows one stale value for a second. */
static char s_title[128] = "Xbox Recomp";

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    char buf[sizeof s_title];
    size_t o = 0;
    int i;

    /* UTF-16 (the certificate's title) to UTF-8 for SDL. BMP only: a lone
     * surrogate becomes '?'. */
    for (i = 0; i < max_chars && name[i]; i++) {
        uint32_t c = name[i];
        if (c >= 0xD800 && c <= 0xDFFF)
            c = '?';
        if (c < 0x80) {
            if (o + 1 >= sizeof buf) break;
            buf[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= sizeof buf) break;
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= sizeof buf) break;
            buf[o++] = (char)(0xE0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    if (o) {
        buf[o] = 0;
        memcpy(s_title, buf, o + 1);
    }
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    SDL_AtomicAdd(&s_flips, 1);
    SDL_AtomicSet(&s_frame_draws, (int)draws);
}

/* ── The window ───────────────────────────────────────────────────────── */

static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e && *e != '0';
}

static SDL_Window   *s_win;
static SDL_Renderer *s_ren;                  /* NULL with an external present */
static Uint32        s_guest_done_event;     /* POSIX: the guest returned */

/* The client area in physical pixels, for an external renderer. 0 while
 * minimised, which that renderer reads as "don't draw". */
static void update_client_size(void)
{
    int w = 0, h = 0;

    if (!(SDL_GetWindowFlags(s_win) & SDL_WINDOW_MINIMIZED))
        SDL_GetWindowSizeInPixels(s_win, &w, &h);
    SDL_AtomicSet(&s_client_wh, (int)(((uint32_t)w & 0xFFFF) << 16 | ((uint32_t)h & 0xFFFF)));
}

static void set_fullscreen(int on)
{
    SDL_SetWindowFullscreen(s_win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    update_client_size();
}

#if defined(_WIN32)
/* DPI: the process is DPI-unaware (Windows scales its windows as bitmaps,
 * which blurs the picture), so only the window's thread opts in, per
 * monitor, before the window is made, as the GDI window does. Looked up at
 * run time: older Windows and Wine may lack them. */
typedef HANDLE (WINAPI *set_thread_dpi_fn)(HANDLE);
typedef UINT   (WINAPI *get_dpi_window_fn)(HWND);

static void dpi_aware_thread(void)
{
    HMODULE u32 = GetModuleHandleA("user32.dll");
    set_thread_dpi_fn set_dpi = NULL;

    if (u32)
        set_dpi = (set_thread_dpi_fn)(void *)GetProcAddress(u32, "SetThreadDpiAwarenessContext");
    /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    if (set_dpi)
        set_dpi((HANDLE)(INT_PTR)-4);
}

static UINT window_dpi(HWND hwnd)
{
    HMODULE u32 = GetModuleHandleA("user32.dll");
    get_dpi_window_fn get_dpi = u32 ? (get_dpi_window_fn)(void *)GetProcAddress(
                                          u32, "GetDpiForWindow") : NULL;
    UINT dpi = get_dpi ? get_dpi(hwnd) : 96;
    return dpi ? dpi : 96;
}
#endif

/* Opens SDL, the window and (unless external) its renderer. 0 = no window;
 * everything it took is given back. */
static int window_open(void)
{
    const char *e;
    int vsync, scale;
    unsigned dpi = 96;

    e = getenv("RECOMP_PRESENT_VSYNC");
    vsync = !e || env_on("RECOMP_PRESENT_VSYNC");
#if defined(_WIN32)
    const int scale_default = 1;
#else
    const int scale_default = 2;
#endif
    e = getenv("RECOMP_WINDOW_SCALE");
    scale = e ? atoi(e) : scale_default;
    if (scale < 1 || scale > 8)
        scale = scale_default;

#if defined(_WIN32)
    dpi_aware_thread();
#endif
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    if (SDL_Init(FBWIN_SDL_SUBSYSTEMS) != 0) {
        fprintf(stderr, "  [FBWIN] SDL video unavailable (%s); running headless\n",
                SDL_GetError());
        return 0;
    }
    s_win = SDL_CreateWindow(s_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             (int)FB_W * scale, (int)FB_H * scale,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
                             | SDL_WINDOW_HIDDEN);
    s_ren = s_win && !s_external
          ? SDL_CreateRenderer(s_win, -1, vsync ? SDL_RENDERER_PRESENTVSYNC : 0)
          : NULL;
    s_guest_done_event = s_win && (s_ren || s_external) ? SDL_RegisterEvents(2)
                                                        : (Uint32)-1;
    if (s_guest_done_event == (Uint32)-1) {
        fprintf(stderr, "  [FBWIN] SDL window/renderer failed (%s); running headless\n",
                SDL_GetError());
        if (s_ren)
            SDL_DestroyRenderer(s_ren);
        if (s_win)
            SDL_DestroyWindow(s_win);
        s_ren = NULL;
        s_win = NULL;
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return 0;
    }
    s_frame_event = s_guest_done_event + 1;
    if (s_ren)
        SDL_RenderSetLogicalSize(s_ren, (int)FB_W, (int)FB_H);

    {
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        if (SDL_GetWindowWMInfo(s_win, &wm)) {
#if defined(_WIN32)
            if (wm.subsystem == SDL_SYSWM_WINDOWS) {
                /* Same physical size as 640x480 at 100%, on a scaled
                 * monitor too. */
                dpi = window_dpi(wm.info.win.window);
                if (dpi != 96)
                    SDL_SetWindowSize(s_win, MulDiv((int)FB_W * scale, (int)dpi, 96),
                                      MulDiv((int)FB_H * scale, (int)dpi, 96));
                SDL_SetWindowPosition(s_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                s_hwnd = wm.info.win.window;
            }
#endif
        }
    }
    SDL_ShowWindow(s_win);
    if (env_on("RECOMP_FB_FULLSCREEN"))
        set_fullscreen(1);
    update_client_size();

    {
        SDL_RendererInfo ri;
        if (s_ren && SDL_GetRendererInfo(s_ren, &ri) == 0)
            fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL %s renderer,"
                    " vsync %s, dpi %u)\n", FB_W, FB_H, ri.name, vsync ? "on" : "off", dpi);
        else
            fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL, external"
                    " present, dpi %u)\n", FB_W, FB_H, dpi);
    }
    xbox_InputSdlInit();
    return 1;
}

/* The event loop and the presents, until the window is closed (returns 1)
 * or the guest returns (POSIX, returns 0). */
static int window_loop(void)
{
    SDL_Texture *tex = NULL;
    uint32_t tex_bpp = 0;
    uint64_t shown_seq = 0, t_title = 0;
    int flips_title = 0, quit = 0, guest_done = 0, dirty = 1, fullscreen;

    fullscreen = (SDL_GetWindowFlags(s_win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    while (!quit && !guest_done) {
        SDL_Event ev;
        uint64_t seq, now;
        const uint8_t *px = NULL;
        uint32_t bpp = 0, pitch = 0;

        /* Before the first flip, poll at the display rate: the window then
         * reads the display surface itself. After, the flip's event wakes
         * the loop. */
        if (SDL_WaitEventTimeout(&ev, shown_seq || s_external ? 100 : 16)) {
            do {
                /* Alt+Enter: fullscreen, not Start (the Enter key-up still
                 * goes through and just releases a key that isn't held). */
                if (ev.type == SDL_KEYDOWN && (ev.key.keysym.mod & KMOD_ALT)
                        && (ev.key.keysym.sym == SDLK_RETURN
                            || ev.key.keysym.sym == SDLK_KP_ENTER)) {
                    if (!ev.key.repeat) {
                        fullscreen = !fullscreen;
                        set_fullscreen(fullscreen);
                    }
                    continue;
                }
                /* Keys for the pad stand-in, and the window events that
                 * release them; it only reads the event. */
                xbox_InputSdlEvent(&ev);
                if (ev.type == SDL_QUIT)
                    quit = 1;
                else if (ev.type == s_guest_done_event)
                    guest_done = 1;
                else if (ev.type == s_frame_event)
                    SDL_AtomicSet(&s_wake_pending, 0);
                else if (ev.type == SDL_WINDOWEVENT) {
                    switch (ev.window.event) {
                    case SDL_WINDOWEVENT_EXPOSED:
                        dirty = 1;
                        break;
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                    case SDL_WINDOWEVENT_MINIMIZED:
                    case SDL_WINDOWEVENT_RESTORED:
                    case SDL_WINDOWEVENT_MAXIMIZED:
                    case SDL_WINDOWEVENT_DISPLAY_CHANGED:
                        update_client_size();
                        dirty = 1;
                        break;
                    }
                }
            } while (SDL_PollEvent(&ev));
        }

        if (s_ren) {
            SDL_AtomicLock(&s_lock);
            seq = s_ready_seq;
            if (seq != shown_seq) {
                int t = s_front;
                s_front = s_ready;
                s_ready = t;
            }
            SDL_AtomicUnlock(&s_lock);

            if (seq != shown_seq) {
                shown_seq = seq;
                px = s_slot[s_front].px;
                bpp = s_slot[s_front].bpp;
                pitch = FB_W * bpp;
            } else if (!shown_seq && s_fb_va && s_fb_pitch) {
                /* No flip yet: the display surface, as the GDI window reads it. */
                bpp = s_fb_pitch / FB_W;
                pitch = s_fb_pitch;
                if (bpp == 2 || bpp == 4)
                    px = (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            }
            if (px) {
                if (!tex || bpp != tex_bpp) {
                    if (tex)
                        SDL_DestroyTexture(tex);
                    tex = SDL_CreateTexture(s_ren, bpp == 4 ? SDL_PIXELFORMAT_RGB888
                                                            : SDL_PIXELFORMAT_RGB565,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            (int)FB_W, (int)FB_H);
                    tex_bpp = bpp;
                }
                if (tex)
                    SDL_UpdateTexture(tex, NULL, px, (int)pitch);
                dirty = 1;
            }
            if (dirty) {
                dirty = 0;
                SDL_SetRenderDrawColor(s_ren, 0, 0, 0, 255);
                SDL_RenderClear(s_ren);
                if (tex)
                    SDL_RenderCopy(s_ren, tex, NULL, NULL);
                SDL_RenderPresent(s_ren);
            }
        }

        now = SDL_GetTicks64();
        if (now - t_title >= 1000) {
            int f = SDL_AtomicGet(&s_flips);
            char tb[192];
            snprintf(tb, sizeof tb, "%s | FPS: %.1f | draws: %u", s_title,
                     t_title ? (double)(f - flips_title) * 1000.0 / (double)(now - t_title)
                             : 0.0,
                     (unsigned)SDL_AtomicGet(&s_frame_draws));
            SDL_SetWindowTitle(s_win, tb);
            t_title = now;
            flips_title = f;
        }
    }
    if (tex)
        SDL_DestroyTexture(tex);
    return quit;
}

/* No present may be mid-publish (it pushes an SDL event) once SDL goes. */
static void stop_presents(void)
{
    SDL_AtomicSet(&s_running, 0);
    while (SDL_AtomicGet(&s_in_present))
        SDL_Delay(1);
}

#if defined(_WIN32)

/* ── Windows: the window on a thread of its own, as the GDI window ──────── */

static SDL_atomic_t s_started;

static DWORD WINAPI fb_thread(LPVOID unused)
{
    (void)unused;
    if (!window_open()) {
        SDL_AtomicSet(&s_started, 0);
        return 0;
    }
    SDL_AtomicSet(&s_running, 1);
    window_loop();                     /* only ever returns on close */
    stop_presents();
    /* Terminate rather than ExitProcess: see fb_present.c's WM_CLOSE. */
    fprintf(stderr, "  [FBWIN] window closed, exiting\n");
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW") || env_on("RECOMP_HEADLESS"))
        return;
    if (SDL_AtomicCAS(&s_started, 0, 1) == SDL_FALSE)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th) {
        xbox_name_thread(th, "fb window");
        CloseHandle(th);
    } else {
        SDL_AtomicSet(&s_started, 0);
    }
}

#else /* POSIX */

/* The window is opened by xbox_HostWindowMain, on the main thread. */
void xbox_FramebufferWindowStart(void) {}

/* ── The guest thread ─────────────────────────────────────────────────── */

static int (*s_guest_main)(void);
static volatile int s_guest_rc;

static void *guest_thread(void *arg)
{
    SDL_Event ev;

    (void)arg;
    s_guest_rc = s_guest_main();
    memset(&ev, 0, sizeof ev);
    ev.type = s_guest_done_event;
    SDL_PushEvent(&ev);
    return NULL;
}

static int start_guest(void)
{
    pthread_attr_t attr;
    pthread_t th;
    struct rlimit rl;
    size_t stack = 64u << 20;
    int rc;

    /* At least the main thread's stack (8 MB by default), and generously
     * more: lifted code recurses on the native stack. */
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
#if defined(__APPLE__)
    /* Same scheduling class as the thread it replaces, so moving the guest
     * off the main thread does not move it to the efficiency cores. */
    {
        qos_class_t qos;
        int rel;
        if (pthread_get_qos_class_np(pthread_self(), &qos, &rel) == 0
                && qos != QOS_CLASS_UNSPECIFIED)
            pthread_attr_set_qos_class_np(&attr, qos, rel);
    }
#endif
    rc = pthread_create(&th, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

int xbox_HostWindowMain(int (*guest_main)(void))
{
    int quit;

    /* Ctrl-C and SIGTERM keep their default meaning; SDL would turn them
     * into an SDL_QUIT that a headless run never reads. Set before the
     * headless return: audio and input start SDL subsystems there too. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!env_on("RECOMP_FB_WINDOW") || env_on("RECOMP_HEADLESS"))
        return guest_main();
    if (!window_open())
        return guest_main();

    s_guest_main = guest_main;
    /* This thread pumps SDL events from here on, which also updates the
     * game controllers; the input layer then only reads them. */
    xbox_InputSetMainPump(1);
    SDL_AtomicSet(&s_running, 1);
    if (!start_guest()) {
        fprintf(stderr, "  [FBWIN] cannot start the guest thread; running headless\n");
        SDL_AtomicSet(&s_running, 0);
        xbox_InputSetMainPump(0);
        if (s_ren)
            SDL_DestroyRenderer(s_ren);
        SDL_DestroyWindow(s_win);
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
        return guest_main();
    }

    quit = window_loop();
    stop_presents();
    fprintf(stderr, "  [FBWIN] %s\n", quit ? "window closed" : "guest returned");
    fflush(stdout);
    fflush(stderr);

    /* Window and renderer go first, on this thread as AppKit wants. SDL_Quit
     * is only safe once nothing else uses SDL: the USB and guest threads read
     * the game controllers, so with one open SDL stays up and exit()
     * reclaims it. The main-pump flag stays set, so those threads do not
     * start calling SDL_GameControllerUpdate while SDL goes down. */
    if (s_ren)
        SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(s_win);
    if (!xbox_InputAnyOpen())
        SDL_Quit();
    else
        SDL_QuitSubSystem(FBWIN_SDL_SUBSYSTEMS);
    if (quit)
        exit(0);
    return s_guest_rc;
}

#endif /* _WIN32 */

#endif /* !_WIN32 || XBOX_SDL_WINDOW */
