/**
 * mkb_input.h -- mouse + keyboard as a virtual Xbox gamepad.
 *
 * The goal: let a recompiled title be played like a native PC FPS --
 * raw mouse deltas drive the aim stick, WASD drives movement, mouse
 * buttons drive the triggers -- regardless of which path the title
 * takes to read its pad (the emulated USB device or the kernel's
 * XInput ordinals), because the merge happens in xbox_InputGetState,
 * which both go through.
 *
 * All knobs are env vars (and an optional mkb.ini), so no rebuild is
 * needed to tune the feel:
 *
 *   RECOMP_MKB=0|1        master switch            (default off, opt-in)
 *   RECOMP_MKB_SENS=f     mouse sensitivity        (default 1.0)
 *   RECOMP_MKB_FULL=n     mouse counts for a full  (default 8)
 *                         stick throw, per poll
 *   RECOMP_MKB_DZ=f       assumed game deadzone    (default 0.18)
 *   RECOMP_MKB_ACC=f      aim acceleration         (default 1.0 = off)
 *   RECOMP_MKB_INVERT_Y=1 invert vertical aim      (default off)
 *   RECOMP_MKB_INI=path   config file (same keys, one per line)
 *
 * F9 toggles mouse/keyboard capture at runtime. Capture is only
 * applied while the recompiled game's process has the foreground
 * window (GDI build) or the SDL window has the keyboard focus (SDL
 * build: relative mouse mode), so the mouse keeps working normally in
 * other apps.
 */

#ifndef XBOXRECOMP_MKB_INPUT_H
#define XBOXRECOMP_MKB_INPUT_H

#include <stdint.h>
#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when the layer is enabled (RECOMP_MKB not "0"). Cheap; cached. */
int mkb_enabled(void);

/*
 * Merge the mouse+keyboard virtual pad on top of *pState, in place.
 * Buttons OR in, analog buttons take the larger value, and the aim
 * stick is taken over by the mouse whenever the mouse moved since the
 * last poll. Bumps dwPacketNumber when the virtual pad contributed
 * anything, so a title watching for edges sees the presses.
 *
 * Returns 1 when the virtual pad is active and produced a usable
 * state (so a caller treating "no host pad" as an error can still
 * report a connected controller), 0 otherwise.
 */
int mkb_merge(DWORD dwPort, XBOX_INPUT_STATE *pState);

/*
 * An overlay (the settings menu) owns the mouse and keys while open:
 * capture is released and the virtual pad reads at rest. Any thread.
 */
void mkb_set_overlay(int open);

#if !defined(_WIN32) || defined(XBOX_SDL_WINDOW)
/*
 * SDL build: the window's thread (src/video/fb_present_sdl.c) hands every
 * event here, and returns early when this returns nonzero (the event was
 * the pad's: captured mouse motion, buttons, wheel, F9). Keys are read and
 * passed on. mkb_sdl_tick runs once per loop pass, after the events, and
 * turns SDL's relative mouse mode on or off; both only on that thread.
 */
union SDL_Event;
int  mkb_sdl_event(const union SDL_Event *ev);
void mkb_sdl_tick(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_MKB_INPUT_H */
