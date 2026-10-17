/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32                 -> Windows XInput
 *   POSIX, XBOX_SDL_WINDOW -> SDL2 GameController (on Windows with the SDL
 *                             window, so one backend serves every host)
 */

#include "xinput_xbox.h"
#include "keyboard.h"
#include "mkb_input.h"
#include <stdlib.h>
#include <string.h>

/* Merge the keyboard on top of a pad on port 0 rather than only standing in
 * for a missing one.
 *
 * The first version put the keyboard behind XInput's failure, on the
 * assumption that with nothing plugged in the call would fail. It does
 * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
 * with every button at rest, so the fallback was unreachable and
 * pressing a key did nothing at all.
 *
 * Merging is also the better rule. A real pad keeps working -- its
 * buttons are already in pState and the keyboard only adds to them --
 * and there is no special case left to get wrong. */
static void keyboard_merge(XBOX_INPUT_STATE *pState)
{
    XBOX_INPUT_STATE kb;
    int i;

    if (!xbox_InputKeyboardPoll(&kb))
        return;
    pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
    for (i = 0; i < 8; i++)
        if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
            pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
    if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
    if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
    if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
    if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
    /* The input layer records edges, so an unchanged packet number is
     * read as the same state and the press never happens. */
    pState->dwPacketNumber = kb.dwPacketNumber;
}

/* ======================================================================== */
#if defined(_WIN32) && !defined(XBOX_SDL_WINDOW)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

/* The keyboard on port 0 (RECOMP_KEYBOARD=1) comes from keyboard.c, shared
 * with the SDL backend; the GDI window in fb_present.c records the keys. */

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0) {
            /* Keyboard and mouse stand in for an absent pad, so the game is
             * playable with no controller plugged in. */
            int kb;
            memset(pState, 0, sizeof *pState);
            kb = xbox_InputKeyboardPoll(pState);
            if (mkb_merge(dwPort, pState))
                g_controller_connected[dwPort] = TRUE;
            if (kb || mkb_enabled())
                return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    if (dwPort == 0)
        keyboard_merge(pState);

    /* Mouse+keyboard merges on top of a real pad too: the mouse owns the
     * aim stick while it is moving, WASD owns movement while held. */
    if (dwPort == 0)
        mkb_merge(dwPort, pState);

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 || XBOX_SDL_WINDOW */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>
#include <stdio.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];
static BOOL  g_combo[XBOX_MAX_CONTROLLERS];   /* Back+Start held */

/* The pads may be read from more than one thread (the USB thread, the
 * kernel's XInput ordinals), and a rescan closes and opens them. */
static SDL_SpinLock g_lock;
static SDL_atomic_t g_init;                   /* 0 no, 1 going, 2 up, 3 failed */
static Uint64       g_last_scan;

/* Set while the framebuffer window's main thread pumps SDL events
 * (src/video/fb_present_sdl.c). The pump already updates the controllers,
 * and SDL_GameControllerUpdate pumps events itself, which on macOS must only
 * happen on the main thread; so then this side only reads them. Windows
 * never sets it: the window runs on a thread of its own there, and the pads
 * update here (SDL's joystick calls take its joystick lock). */
static volatile int g_main_pump;

void xbox_InputSetMainPump(int on)
{
    g_main_pump = on;
}

/* Whether any controller is open: SDL must stay up for it at shutdown. */
int xbox_InputAnyOpen(void)
{
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++)
        if (g_pads[i])
            return 1;
    return 0;
}

/* Drop the pads that went away, then give each newly attached game
 * controller the first free port, in SDL's device order. With g_lock held. */
static void open_controllers(void)
{
    int slot, i, n;

    for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot] && !SDL_GameControllerGetAttached(g_pads[slot])) {
            SDL_GameControllerClose(g_pads[slot]);
            g_pads[slot] = NULL;
            g_controller_connected[slot] = FALSE;
            fprintf(stderr, "  [PAD] port %d: disconnected\n", slot);
        }
    }
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) {
        SDL_JoystickID id;
        int open = 0;

        if (!SDL_IsGameController(i))
            continue;
        id = SDL_JoystickGetDeviceInstanceID(i);
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
            if (g_pads[slot] && SDL_JoystickInstanceID(
                    SDL_GameControllerGetJoystick(g_pads[slot])) == id)
                open = 1;
        if (open)
            continue;
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS && g_pads[slot]; slot++)
            ;
        if (slot == XBOX_MAX_CONTROLLERS)
            break;
        g_pads[slot] = SDL_GameControllerOpen(i);
        if (g_pads[slot]) {
            const char *name = SDL_GameControllerName(g_pads[slot]);
            fprintf(stderr, "  [PAD] port %d: %s (SDL game controller)\n",
                    slot, name ? name : "unnamed");
        }
    }
    fflush(stderr);
    g_last_scan = SDL_GetTicks64();
}

/* Once, on whichever thread gets here first; the others wait for "up" or
 * "failed". SDL_InitSubSystem is not thread-safe, so a title calls
 * xbox_InputInit on its main thread before other threads use SDL; an input
 * thread getting here first is the fallback. */
static int input_up(void)
{
    int st = SDL_AtomicGet(&g_init);

    if (st >= 2)
        return st == 2;
    if (SDL_AtomicCAS(&g_init, 0, 1)) {
        /* Keep the default SIGINT/SIGTERM: SDL would turn them into an
         * SDL_QUIT that nothing reads without a window, and Ctrl-C would
         * stop working. */
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#if defined(_WIN32)
        /* Read as XInput was: also while the window is in the background.
         * Device arrival goes to a thread of SDL's own: the thread that
         * initialises SDL here is the game's and never pumps messages. */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
#endif
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER)
                || SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
#if defined(_WIN32)
            /* The state is polled below; the pads' events would only pile
             * up for the window thread to drop. */
            SDL_GameControllerEventState(SDL_IGNORE);
            SDL_JoystickEventState(SDL_IGNORE);
#endif
            SDL_AtomicLock(&g_lock);
            open_controllers();
            SDL_AtomicUnlock(&g_lock);
            SDL_AtomicSet(&g_init, 2);
        } else {
            fprintf(stderr, "  [PAD] SDL game controllers unavailable (%s)\n",
                    SDL_GetError());
            SDL_AtomicSet(&g_init, 3);
        }
        return SDL_AtomicGet(&g_init) == 2;
    }
    while ((st = SDL_AtomicGet(&g_init)) == 1)
        SDL_Delay(1);
    return st == 2;
}

void xbox_InputInit(void)
{
    input_up();
}

/* No pad on this port: on port 0 the keyboard (and the mouse+keyboard
 * pad) stand in, so the game is playable with none. */
static DWORD no_pad(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    g_controller_connected[dwPort] = FALSE;
    if (dwPort == 0) {
        int kb;
        memset(pState, 0, sizeof *pState);
        kb = xbox_InputKeyboardPoll(pState);
        if (mkb_merge(dwPort, pState))
            g_controller_connected[dwPort] = TRUE;
        if (kb || mkb_enabled())
            return ERROR_SUCCESS;
    }
    return ERROR_DEVICE_NOT_CONNECTED;
}

static DWORD get_state(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    SDL_GameController *c;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!input_up())
        return no_pad(dwPort, pState);

    SDL_AtomicLock(&g_lock);
    if (!g_main_pump)
        SDL_GameControllerUpdate();
    c = g_pads[dwPort];
    /* Plugged in or pulled out: look again, at most once a second. */
    if ((!c || !SDL_GameControllerGetAttached(c))
            && SDL_GetTicks64() - g_last_scan >= 1000) {
        open_controllers();
        c = g_pads[dwPort];
    }
    if (!c || !SDL_GameControllerGetAttached(c)) {
        SDL_AtomicUnlock(&g_lock);
        g_combo[dwPort] = FALSE;
        return no_pad(dwPort, pState);
    }
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
    SDL_AtomicUnlock(&g_lock);

    /* Back+Start held together on the pad itself (a title's menu combo),
     * logged as it is pressed, before the keyboard can add either. */
    {
        const WORD both = XBOX_GAMEPAD_BACK | XBOX_GAMEPAD_START;
        BOOL held = (btn & both) == both;
        if (held && !g_combo[dwPort]) {
            fprintf(stderr, "  [PAD] port %lu: Back+Start held\n", (unsigned long)dwPort);
            fflush(stderr);
        }
        g_combo[dwPort] = held;
    }

    if (dwPort == 0)
        keyboard_merge(pState);
    /* Mouse+keyboard merges on top of a real pad too, as with XInput. */
    if (dwPort == 0)
        mkb_merge(dwPort, pState);
    return ERROR_SUCCESS;
}

static xbox_input_filter_fn volatile g_filter;

void xbox_InputSetFilter(xbox_input_filter_fn fn)
{
    g_filter = fn;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    DWORD rc = get_state(dwPort, pState);
    xbox_input_filter_fn fn = g_filter;

    if (rc == ERROR_SUCCESS && fn)
        fn(dwPort, pState);
    return rc;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    SDL_GameController *c;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!input_up())
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_AtomicLock(&g_lock);
    c = g_pads[dwPort];
    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    if (c)
        SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                                 pVibration->wRightMotorSpeed, 1000);
    SDL_AtomicUnlock(&g_lock);
    return c ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 && !XBOX_SDL_WINDOW */
