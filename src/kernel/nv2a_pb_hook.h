/**
 * Hand the decoded pushbuffer to an external renderer.
 *
 * nv2a_pb_scan decodes the title's pushbuffer on the ack thread and calls
 * nv2a_pb_exec_method for every (subchannel, method, parameter). A title can
 * register a hook that takes over from there: a port of a full NV2A PGRAPH
 * (e.g. xemu's) that owns all graphics state, including semaphore releases,
 * reports and flips. While a hook is set the software executor does nothing
 * but what the runtime needs around a frame boundary at FLIP_STALL: the
 * title's swap counter (xbox_Nv2aFrameCounterFlip) and the frame capture.
 * Capture recording (nv2a_pb_capture_method) still sees every method first.
 *
 * Set it before the title submits its first method and leave it set: the two
 * executors don't share state, so switching midway loses everything set up so
 * far. The hook runs on the ack thread (or the replay thread).
 */
#ifndef XBOXRECOMP_NV2A_PB_HOOK_H
#define XBOXRECOMP_NV2A_PB_HOOK_H

#include <stdint.h>

typedef void (*nv2a_pb_method_hook_fn)(uint32_t subch, uint32_t method,
                                       uint32_t param);

/* NULL restores the software executor. */
void nv2a_pb_set_method_hook(nv2a_pb_method_hook_fn fn);

/* Called on the hook's thread after each batch of methods: a walk of the
 * pushbuffer up to PUT, or a replayed capture. A hook can then take its lock
 * once per batch instead of once per method (a level sends millions of
 * methods per second). Optional; NULL turns it off. */
typedef void (*nv2a_pb_batch_end_fn)(void);
void nv2a_pb_set_batch_end_hook(nv2a_pb_batch_end_fn fn);

/* A hook that runs software methods (NV097 NO_OPERATION with a non-zero
 * parameter, which on hardware raise an interrupt for D3D's handler) reports
 * each parameter here; the runtime reproduces what D3D's handler would do
 * (kernel_bridge.c). Returns 1 if the method is handled. Any thread. */
int xbox_Nv2aSoftwareMethod(uint32_t param);

/* An emulated vertical blank: sets the event D3D's VBlank DPC would
 * (BlockUntilVerticalBlank's), kernel_bridge.c. Call it once per VBlank from
 * any thread. Returns 0 until D3D has connected its interrupt. */
int xbox_Nv2aVBlank(void);

#endif
