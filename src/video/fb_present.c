/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * A renderer that keeps its frames on the host GPU can instead draw into this
 * window itself (xbox_FramebufferWindowHwnd + xbox_FramebufferWindowGpuPresent);
 * the window thread then only handles messages, focus and keys.
 *
 * The frame is scaled to fit the client area, keeping its aspect, with black
 * bars. Alt+Enter toggles borderless fullscreen on the window's monitor;
 * RECOMP_FB_FULLSCREEN=1 starts that way.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 */
#include <stdint.h>

#if defined(_WIN32) && !defined(XBOX_SDL_WINDOW)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Which keys are down, for the pad stand-in in src/input: this window
 * records them in the shared key table (src/input/keyboard.c).
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them. */
#include "keyboard.h"   /* src/input */
#include "../../include/xbox/thread_name.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;

/* What the window shows: a converted 32-bit copy for GDI and the dump. */
static uint32_t     *s_rgb;
static uint32_t      s_rgb_w, s_rgb_h;
static size_t        s_rgb_cap;      /* pixels */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
typedef struct {
    uint32_t *px;
    size_t    cap;                    /* pixels */
    uint32_t  w, h;
} fb_frame;
static fb_frame      s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

/* Set by a renderer that draws into the window itself: no GDI blit. */
static volatile LONG s_gpu_present;
static volatile HWND s_hwnd;
static volatile LONG s_client_wh;          /* width << 16 | height */

/* The buffer to fill for the next flip, big enough for w x h. A buffer that
 * is too small is replaced and the old one leaked, not freed: two flips
 * during one of the window thread's copies would otherwise have it reading
 * freed memory. Frame sizes change a handful of times per run at most. */
static fb_frame *fb_next_frame(uint32_t w, uint32_t h, LONG *next)
{
    fb_frame *f;
    size_t need = (size_t)w * h;

    *next = (s_present_idx == 0) ? 1 : 0;
    f = &s_present[*next];
    if (f->cap < need) {
        size_t cap = need > (size_t)640 * 480 ? need : (size_t)640 * 480;
        uint32_t *px = (uint32_t *)calloc(cap, 4);
        if (!px)
            return NULL;
        f->px = px;
        f->cap = cap;
    }
    f->w = w;
    f->h = h;
    return f;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Vertical anti-aliasing factor of the surface being shown: 2 for 2x2, where
 * the surface has twice the rows as well as twice the columns, and 1 for
 * everything else (2x1 included). Only the pushbuffer executor knows it. */
static uint32_t s_fb_aa_sy = 1;

void xbox_FramebufferWindowSetAA(uint32_t sy)
{
    s_fb_aa_sy = sy ? sy : 1;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    fb_frame *f;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    f = fb_next_frame(s_fb_width, s_fb_height, &next);
    if (!f)
        return;
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = f->px + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* 2x-wide anti-aliased 32-bit surface: every other pixel, as in
             * fb_convert. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
}

/* A finished frame from an external renderer (nv2a_pb_hook.h), which keeps
 * its surfaces on the host GPU, so there is no guest memory to read: the
 * caller hands over 0x00RRGGBB rows, top-down, of any size. */
void xbox_FramebufferWindowPresentPixels(const uint32_t *xrgb, uint32_t width,
                                         uint32_t height)
{
    fb_frame *f;
    LONG next;

    if (!s_fb_running || !xrgb || !width || !height)
        return;
    f = fb_next_frame(width, height, &next);
    if (!f)
        return;
    memcpy(f->px, xrgb, (size_t)width * height * 4);
    InterlockedExchange(&s_present_idx, next);
}

void *xbox_FramebufferWindowHwnd(void)
{
    return (void *)s_hwnd;
}

/* The GDI window needs no notice: GpuPresent(1) stops its blit. */
void xbox_FramebufferWindowExternalPresent(int on)
{
    (void)on;
}

void xbox_FramebufferWindowGpuPresent(int on)
{
    InterlockedExchange(&s_gpu_present, on ? 1 : 0);
}

int xbox_FramebufferWindowClientSize(uint32_t *width, uint32_t *height)
{
    LONG wh = s_client_wh;

    if (width)
        *width = (uint32_t)(wh >> 16) & 0xFFFF;
    if (height)
        *height = (uint32_t)wh & 0xFFFF;
    return wh != 0;
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
static wchar_t       s_title[48] = L"Xbox Recomp";
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
        s_title[i] = (wchar_t)name[i];
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

/* DPI: the process is DPI-unaware (Windows scales its windows as bitmaps,
 * which blurs the picture), so only this window's thread opts in, per
 * monitor. Looked up at run time: older Windows and Wine may lack them. */
typedef HANDLE (WINAPI *set_thread_dpi_fn)(HANDLE);
typedef UINT   (WINAPI *get_dpi_window_fn)(HWND);
typedef BOOL   (WINAPI *adjust_rect_dpi_fn)(RECT *, DWORD, BOOL, DWORD, UINT);
static get_dpi_window_fn  s_get_dpi_window;
static adjust_rect_dpi_fn s_adjust_rect_dpi;
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

#define FB_STYLE_WINDOWED (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS)
#define FB_STYLE_FULL     (WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS)

/* Borderless fullscreen: a popup covering the monitor the window is on, not
 * a display mode change, so Alt+Tab and other monitors keep working. The
 * windowed placement is saved on the way in and restored on the way out. */
static int             s_fullscreen;
static WINDOWPLACEMENT s_windowed_place;

static void fb_set_fullscreen(HWND h, int on)
{
    if (on == s_fullscreen)
        return;
    if (on) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        s_windowed_place.length = sizeof(s_windowed_place);
        if (!GetWindowPlacement(h, &s_windowed_place) ||
            !GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        SetWindowLongPtrA(h, GWL_STYLE, FB_STYLE_FULL | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongPtrA(h, GWL_STYLE, FB_STYLE_WINDOWED | WS_VISIBLE);
        SetWindowPlacement(h, &s_windowed_place);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    s_fullscreen = on;
    fprintf(stderr, "  [FBWIN] %s\n", on ? "fullscreen" : "windowed");
}

/* The largest rect of the frame's aspect that fits the client area, centred. */
static void fb_fit_rect(const RECT *client, uint32_t fw, uint32_t fh, RECT *out)
{
    LONG cw = client->right - client->left, ch = client->bottom - client->top;
    LONG w = cw, h = ch;

    if (fw && fh) {
        if ((int64_t)cw * fh > (int64_t)ch * fw)
            w = (LONG)((int64_t)ch * fw / fh);    /* bars left and right */
        else
            h = (LONG)((int64_t)cw * fh / fw);    /* bars top and bottom */
    }
    out->left   = client->left + (cw - w) / 2;
    out->top    = client->top + (ch - h) / 2;
    out->right  = out->left + w;
    out->bottom = out->top + h;
}

/* Put s_rgb on the window: aspect-fit, black bars. Bars are filled every time
 * (four FillRects) rather than tracked across resizes. */
static void fb_blit(HWND h, HDC hdc)
{
    RECT client, dst, bar;
    BITMAPINFO bi;
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);

    GetClientRect(h, &client);
    if (!s_rgb || !s_rgb_w || !s_rgb_h) {
        FillRect(hdc, &client, black);
        return;
    }
    fb_fit_rect(&client, s_rgb_w, s_rgb_h, &dst);
    bar = client; bar.right = dst.left;  FillRect(hdc, &bar, black);
    bar = client; bar.left = dst.right;  FillRect(hdc, &bar, black);
    bar = dst; bar.top = client.top;     bar.bottom = dst.top;  FillRect(hdc, &bar, black);
    bar = dst; bar.top = dst.bottom;     bar.bottom = client.bottom; FillRect(hdc, &bar, black);

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_rgb_w;
    bi.bmiHeader.biHeight      = -(LONG)s_rgb_h;    /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    /* HALFTONE averages when shrinking; COLORONCOLOR would drop rows. */
    SetStretchBltMode(hdc, (dst.right - dst.left < (LONG)s_rgb_w) ? HALFTONE
                                                                    : COLORONCOLOR);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchDIBits(hdc, dst.left, dst.top, dst.right - dst.left,
                  dst.bottom - dst.top, 0, 0, (int)s_rgb_w, (int)s_rgb_h,
                  s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
        /* Closing the window quits the title. Only clearing s_fb_running
         * here let the next AvSetDisplayMode / surface setup call
         * xbox_FramebufferWindowStart() and open a fresh window. Terminate
         * rather than ExitProcess: guest threads print all the time, and
         * one killed while holding a CRT stream lock would hang the exit
         * flush. Flush our own output first. */
        fprintf(stderr, "  [FBWIN] window closed, exiting\n");
        fflush(stdout);
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&s_fb_running, 0);
        return 0;

    case WM_SIZE:
        InterlockedExchange(&s_client_wh,
                            (LONG)(((uint32_t)LOWORD(l) << 16) | HIWORD(l)));
        return 0;

    /* A GL renderer covers the whole client area each present; GDI erasing
     * it first would only flash. The blit paints the bars itself. */
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (!s_gpu_present)
            fb_blit(h, dc);
        EndPaint(h, &ps);
        return 0;
    }

    /* Moved to a monitor with another scale: take the size Windows suggests,
     * so the window keeps its physical size. Not while fullscreen, where the
     * rect is the monitor's and the suggestion would shrink it. */
    case WM_DPICHANGED:
        if (!s_fullscreen) {
            const RECT *r = (const RECT *)l;
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* Alt+Enter: fullscreen toggle, not Start. Bit 29 = Alt held,
         * bit 30 = auto-repeat (holding the keys must not flicker). */
        if (m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29))) {
            if (!(l & (1 << 30)))
                fb_set_fullscreen(h, !s_fullscreen);
            return 0;
        }
        xbox_InputKeySet((int)w, 1);
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    /* The character Alt+Enter makes; Windows would beep at it. */
    case WM_SYSCHAR:
        if (w == '\r')
            return 0;
        break;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        xbox_InputKeySet((int)w, 0);
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        xbox_InputKeysClear();
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* Make s_rgb hold w x h pixels (window thread only). */
static int fb_rgb_size(uint32_t w, uint32_t h)
{
    size_t need = (size_t)w * h;

    if (s_rgb_cap < need) {
        uint32_t *p = (uint32_t *)realloc(s_rgb, need * 4);
        if (!p)
            return 0;
        s_rgb = p;
        s_rgb_cap = need;
    }
    s_rgb_w = w;
    s_rgb_h = h;
    return 1;
}

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    if (!fb_rgb_size(s_fb_width, s_fb_height))
        return;
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch
                           * (bpp == 8 ? s_fb_aa_sy : 1);
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 8) {
            /* A 2x-wide anti-aliased 32-bit surface (pitch = 2 * width * 4):
             * take every other pixel. X-Men Legends renders its 640-wide
             * frame into a 1280-wide surface, which read as "8 bytes per
             * pixel" and showed black. */
            const uint32_t *p = (const uint32_t *)row;
            for (x = 0; x < s_fb_width; x++)
                dst[x] = p[x * 2];
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t w = s_rgb_w, hgt = s_rgb_h;
    uint32_t row = ((w * 3u) + 3u) & ~3u;
    uint32_t img = row * hgt, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !w || !hgt)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &hgt, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < hgt; y++) {
        const uint32_t *src = s_rgb + (size_t)(hgt - 1 - y) * w;
        for (x = 0; x < w; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, w, hgt, s_fb_va);
    return 0;
}

static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    RECT r;
    UINT dpi = 96;

    (void)unused;

    {
        HMODULE u32 = GetModuleHandleA("user32.dll");
        set_thread_dpi_fn set_dpi = NULL;
        if (u32) {
            set_dpi = (set_thread_dpi_fn)(void *)GetProcAddress(
                u32, "SetThreadDpiAwarenessContext");
            s_get_dpi_window = (get_dpi_window_fn)(void *)GetProcAddress(
                u32, "GetDpiForWindow");
            s_adjust_rect_dpi = (adjust_rect_dpi_fn)(void *)GetProcAddress(
                u32, "AdjustWindowRectExForDpi");
        }
        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
        if (set_dpi)
            set_dpi((HANDLE)(INT_PTR)-4);
    }
    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        /* CS_OWNDC: a GL context made on this window's DC stays valid. */
        wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    r.left = 0; r.top = 0; r.right = (LONG)s_fb_width; r.bottom = (LONG)s_fb_height;
    AdjustWindowRect(&r, FB_STYLE_WINDOWED, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           FB_STYLE_WINDOWED,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    /* Same physical size as 640x480 at 100%, on a scaled monitor too. */
    if (s_get_dpi_window)
        dpi = s_get_dpi_window(hwnd);
    if (dpi != 96 && dpi && s_adjust_rect_dpi) {
        r.left = 0; r.top = 0;
        r.right = MulDiv((int)s_fb_width, (int)dpi, 96);
        r.bottom = MulDiv((int)s_fb_height, (int)dpi, 96);
        s_adjust_rect_dpi(&r, FB_STYLE_WINDOWED, FALSE, 0, dpi);
        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    {
        const char *fs = getenv("RECOMP_FB_FULLSCREEN");
        ShowWindow(hwnd, SW_SHOW);
        if (fs && *fs && *fs != '0')
            fb_set_fullscreen(hwnd, 1);
    }
    hdc = GetDC(hwnd);
    s_hwnd = hwnd;

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, dpi %u)\n",
            s_fb_width, s_fb_height, dpi);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (s_present_idx >= 0) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            const fb_frame *f = &s_present[s_present_idx];
            if (f->px && fb_rgb_size(f->w, f->h))
                memcpy(s_rgb, f->px, (size_t)f->w * f->h * 4);
            if (!s_gpu_present)
                fb_blit(hwnd, hdc);
        } else if (s_fb_va && s_fb_pitch && !s_gpu_present) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            fb_blit(hwnd, hdc);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 1000) {
                LONG f = s_flips;
                wchar_t tb[128];
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0,
                           (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        Sleep(16);
    }

    s_hwnd = NULL;
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    s_rgb_cap = 0;
    s_rgb_w = s_rgb_h = 0;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th) {
        xbox_name_thread(th, "fb window");
        CloseHandle(th);
    }
    else
        InterlockedExchange(&s_fb_running, 0);
}

#endif /* _WIN32 && !XBOX_SDL_WINDOW: else the SDL2 window in fb_present_sdl.c */
