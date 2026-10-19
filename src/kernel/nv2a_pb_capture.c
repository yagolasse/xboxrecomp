/**
 * See nv2a_pb_capture.h. Implementation of the generic frame capture/replay.
 */
#include "nv2a_pb_capture.h"
#include "nv2a_vsh_interp.h"
#include "xbox_memory_layout.h"
#include "platform/xbox_winnt.h"   /* GetTickCount for the time-based arm */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The software executor and the ack-thread stop, both in the kernel lib. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_batch_end(void);
extern void nv2a_pb_exec_dump_surface(const char *path);
void xbox_Nv2aAckStop(void);

/* The three guest-memory regions a draw reads. The contiguous window is the
 * physical view (XBOX_CONTIG_BASE); the NV2A aperture holds RAMHT/PRAMIN.
 * Both are separate storage from the low 64 MiB (see xbox_memory_layout.c).
 * Main RAM (VA 0, 64 MiB) is the image + guest heap; dma_resolve can fall
 * through to a raw VA there, so a draw may read it directly. */
#define PBCAP_RAM_BASE    0x00000000u
#define PBCAP_RAM_SIZE    (64u * 1024u * 1024u)
#define PBCAP_CONTIG_BASE 0x80000000u
#define PBCAP_CONTIG_SIZE (64u * 1024u * 1024u)
#define PBCAP_NV2A_BASE   0xFD000000u
#define PBCAP_NV2A_SIZE   (16u * 1024u * 1024u)

/* ---- file format ----------------------------------------------------------
 * Header (little-endian, written field by field so no struct padding):
 *   magic[4]    "MKPB"
 *   version     uint32 = 2
 *   ram_size    uint32
 *   contig_size uint32
 *   nv2a_size   uint32
 *   prelude_n   uint32
 *   frame_n     uint32
 *   flip        uint32 (the FLIP_STALL number captured; diagnostic)
 *   contig_hi   uint32 (xbox_ContiguousAllocatedBytes at arm time: dma_resolve
 *                       needs it to map physical offsets the same way)
 * Then, in order: ram blob, contig blob, nv2a blob, prelude_n triples,
 * frame_n triples, where a triple is three uint32s (subch, method, param). */
#define PBCAP_MAGIC "MKPB"
#define PBCAP_VERSION 2u

#define PBCAP_SUBCH 8
#define PBCAP_METHOD_SLOTS 2048   /* method 0..0x1FFC step 4 -> 0..0x7FF */

typedef struct { uint32_t subch, method, param; } PbCapMethod;

static struct {
    int configured;
    int active;
    const char *path;
    uint32_t target;      /* capture the frame ending at the target-th FLIP_STALL */
    uint32_t target_ms;   /* RECOMP_PB_CAPTURE_MS: arm at the first flip >= this
                             many ms after configure; 0 = arming by flip count */
    uint32_t start_ms;    /* GetTickCount() at configure, for target_ms */
    uint32_t flips_seen;  /* FLIP_STALLs processed */
    int armed;            /* snapshots frozen; recording the frame */
    int done;             /* file written */

    /* Shadow: last value written to each (subchannel, method). */
    uint32_t shadow[PBCAP_SUBCH][PBCAP_METHOD_SLOTS];
    uint8_t  valid[PBCAP_SUBCH][PBCAP_METHOD_SLOTS];

    /* Snapshots taken at arm time. */
    uint8_t *ram;
    uint8_t *contig;
    uint8_t *nv2a;
    uint32_t contig_hi;   /* xbox_ContiguousAllocatedBytes at arm time */

    /* Prelude (built at arm time) and frame (accumulated while armed). */
    PbCapMethod *prelude;
    uint32_t prelude_n, prelude_cap;
    PbCapMethod *frame;
    uint32_t frame_n, frame_cap;
} s;

static void oom(void)
{
    fprintf(stderr, "[PBCAP] out of memory\n");
    exit(1);
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q && n) oom();
    return q;
}

static void reserve(PbCapMethod **buf, uint32_t *cap, uint32_t need)
{
    uint32_t ncap;
    if (need <= *cap)
        return;
    ncap = *cap ? *cap : 4096;
    while (ncap < need)
        ncap *= 2;
    *buf = xrealloc(*buf, (size_t)ncap * sizeof(PbCapMethod));
    *cap = ncap;
}

static void append(PbCapMethod **buf, uint32_t *cap, uint32_t *n,
                   uint32_t subch, uint32_t method, uint32_t param)
{
    reserve(buf, cap, *n + 1);
    (*buf)[*n].subch  = subch;
    (*buf)[*n].method = method;
    (*buf)[*n].param  = param;
    (*n)++;
}

/* Methods whose "last value" does not describe state: they are excluded from
 * the shadow/prelude but still appear in the frame stream. */
static int is_action_method(uint32_t method)
{
    /* Vertex-program upload: data words go into an auto-incrementing slot, so
     * "last value per method" cannot describe them; the prelude re-uploads the
     * whole program and constant file explicitly. The load/start/context
     * pointers are replayed explicitly too. */
    if (method >= 0x0B00 && method < 0x0C00) return 1;
    if (method >= 0x1E98 && method <= 0x1EA4) return 1;
    /* Geometry submission. */
    if (method == 0x17FC) return 1;                     /* SET_BEGIN_END */
    if (method == 0x1800) return 1;                     /* ARRAY_ELEMENT16 */
    if (method == 0x1810) return 1;                     /* DRAW_ARRAYS */
    if (method == 0x1818) return 1;                     /* INLINE_ARRAY */
    if (method >= 0x1880 && method < 0x1B00) return 1;  /* immediate vertex data */
    /* Presentation and sync side effects. */
    if (method == 0x1D94) return 1;                     /* CLEAR_SURFACE */
    if (method == 0x1D70) return 1;                     /* BACK_END_WRITE_SEMAPHORE_RELEASE */
    if (method == 0x012C) return 1;                     /* FLIP_INCREMENT_WRITE */
    if (method == 0x0130) return 1;                     /* FLIP_STALL */
    if (method == 0x17D0) return 1;                     /* GET_REPORT */
    return 0;
}

static void configure(void)
{
    const char *p = getenv("RECOMP_PB_CAPTURE");
    const char *f;
    s.configured = 1;
    if (!p || !p[0]) {
        s.active = 0;
        return;
    }
    s.active = 1;
    s.path = p;
    s.target = 1;
    s.target_ms = 0;
    s.start_ms = (uint32_t)GetTickCount();
    f = getenv("RECOMP_PB_CAPTURE_FLIP");
    if (f && f[0]) {
        long n = atol(f);
        if (n > 0)
            s.target = (uint32_t)n;
    }
    /* Time-based arming overrides the flip count. The flip count is not
     * reproducible: frames before the level run uncapped, so it depends on
     * how fast the host drew the title and movies. Arming on elapsed time
     * matches a time-based trigger (a scripted pad event) and is stable. */
    f = getenv("RECOMP_PB_CAPTURE_MS");
    if (f && f[0]) {
        long n = atol(f);
        if (n > 0)
            s.target_ms = (uint32_t)n;
    }
    if (s.target_ms)
        fprintf(stderr, "[PBCAP] capturing first flip after %u ms to %s\n",
                s.target_ms, s.path);
    else
        fprintf(stderr, "[PBCAP] capturing flip %u to %s\n", s.target, s.path);
}

/* Freeze the two memory regions and build the prelude from the shadow table
 * plus the vertex-program interpreter's current state. */
static void arm(void)
{
    uint32_t subch, mi, i;
    uint8_t *base = (uint8_t *)xbox_GetMemoryBase();

    s.armed = 1;
    s.frame_n = 0;

    s.ram    = xrealloc(s.ram,    PBCAP_RAM_SIZE);
    s.contig = xrealloc(s.contig, PBCAP_CONTIG_SIZE);
    s.nv2a   = xrealloc(s.nv2a,   PBCAP_NV2A_SIZE);
    s.contig_hi = xbox_ContiguousAllocatedBytes();
    memcpy(s.ram,    base + PBCAP_RAM_BASE,    PBCAP_RAM_SIZE);
    memcpy(s.contig, base + PBCAP_CONTIG_BASE, PBCAP_CONTIG_SIZE);
    memcpy(s.nv2a,   base + PBCAP_NV2A_BASE,   PBCAP_NV2A_SIZE);

    /* Ordinary state: every (subchannel, method) the title has written. */
    s.prelude_n = 0;
    for (subch = 0; subch < PBCAP_SUBCH; subch++)
        for (mi = 0; mi < PBCAP_METHOD_SLOTS; mi++) {
            uint32_t method = mi * 4;
            if (!s.valid[subch][mi] || is_action_method(method))
                continue;
            append(&s.prelude, &s.prelude_cap, &s.prelude_n,
                   subch, method, s.shadow[subch][mi]);
        }

    /* Vertex program: re-upload the whole 136-slot program memory, then the
     * start slot, context-write enable, and all 192 constants. */
    append(&s.prelude, &s.prelude_cap, &s.prelude_n, 0, 0x1E9C, 0); /* _PROGRAM_LOAD 0 */
    for (i = 0; i < NV2A_VSH_SLOTS * 4; i++)
        append(&s.prelude, &s.prelude_cap, &s.prelude_n,
               0, 0x0B00, nv2a_vsh_program_read(i / 4, i % 4));
    append(&s.prelude, &s.prelude_cap, &s.prelude_n, 0, 0x1EA0,
           nv2a_vsh_start_slot_get());
    append(&s.prelude, &s.prelude_cap, &s.prelude_n, 0, 0x1E98,
           nv2a_vsh_cxt_write_get());
    append(&s.prelude, &s.prelude_cap, &s.prelude_n, 0, 0x1EA4, 0); /* _CONSTANT_LOAD 0 */
    for (i = 0; i < NV2A_VSH_CONSTANTS * 4; i++)
        append(&s.prelude, &s.prelude_cap, &s.prelude_n,
               0, 0x0B80, nv2a_vsh_constant_read(i / 4, i % 4));

    fprintf(stderr, "[PBCAP] armed at flip %u: %u prelude methods\n",
            s.flips_seen, s.prelude_n);
}

static void write_file(void)
{
    char path[1024];
    FILE *f;
    uint32_t u32;

    f = fopen(s.path, "wb");
    if (!f) {
        fprintf(stderr, "[PBCAP] cannot open %s\n", s.path);
        return;
    }
    fwrite(PBCAP_MAGIC, 1, 4, f);
    u32 = PBCAP_VERSION;    fwrite(&u32, 4, 1, f);
    u32 = PBCAP_RAM_SIZE;   fwrite(&u32, 4, 1, f);
    u32 = PBCAP_CONTIG_SIZE; fwrite(&u32, 4, 1, f);
    u32 = PBCAP_NV2A_SIZE;  fwrite(&u32, 4, 1, f);
    u32 = s.prelude_n;      fwrite(&u32, 4, 1, f);
    u32 = s.frame_n;        fwrite(&u32, 4, 1, f);
    u32 = s.flips_seen;     fwrite(&u32, 4, 1, f);
    u32 = s.contig_hi;      fwrite(&u32, 4, 1, f);
    fwrite(s.ram,    1, PBCAP_RAM_SIZE,    f);
    fwrite(s.contig, 1, PBCAP_CONTIG_SIZE, f);
    fwrite(s.nv2a,   1, PBCAP_NV2A_SIZE,   f);
    fwrite(s.prelude, sizeof(PbCapMethod), s.prelude_n, f);
    fwrite(s.frame,   sizeof(PbCapMethod), s.frame_n,   f);
    fclose(f);

    /* Reference frame for the parity check, dumped at the same boundary. */
    snprintf(path, sizeof path, "%s.ref.bmp", s.path);
    nv2a_pb_exec_dump_surface(path);

    fprintf(stderr, "[PBCAP] wrote %s (flip %u, %u frame methods)\n",
            s.path, s.flips_seen, s.frame_n);
}

void nv2a_pb_capture_method(uint32_t subch, uint32_t method, uint32_t param)
{
    if (!s.configured)
        configure();
    if (!s.active || s.done)
        return;

    /* Capture the very first frame lazily: arm before its first method.
     * Only in flip-count mode -- a time-based arm waits for the flip. */
    if (!s.armed && !s.target_ms && s.target == 1)
        arm();

    /* Mirror state for the prelude. */
    if (subch < PBCAP_SUBCH && (method >> 2) < PBCAP_METHOD_SLOTS
            && !is_action_method(method)) {
        s.shadow[subch][method >> 2] = param;
        s.valid[subch][method >> 2] = 1;
    }

    if (s.armed)
        append(&s.frame, &s.frame_cap, &s.frame_n, subch, method, param);
}

void nv2a_pb_capture_flip(void)
{
    if (!s.configured)
        configure();
    if (!s.active || s.done)
        return;

    s.flips_seen++;

    /* A whole frame has passed since arm(): write it. arm() runs at the
     * boundary before the recorded frame, so this fires one flip later. */
    if (s.armed) {
        write_file();
        s.done = 1;
    } else if (s.target_ms) {
        /* Time-based: freeze at the first flip boundary on/after T ms. */
        if ((int32_t)(GetTickCount() - s.start_ms) >= (int32_t)s.target_ms)
            arm();
    } else if (s.target > 1 && s.flips_seen == s.target - 1) {
        arm();
    }
}

int nv2a_pb_capture_replay(const char *path)
{
    FILE *f;
    char magic[4];
    uint32_t version, ram_size, contig_size, nv2a_size, prelude_n, frame_n, flip, i;
    uint32_t contig_hi;
    PbCapMethod *prelude = NULL, *frame = NULL;
    uint8_t *blob = NULL;
    char bmp[1024];
    uint8_t *base = (uint8_t *)xbox_GetMemoryBase();

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[PBCAP] replay: cannot open %s\n", path);
        return 1;
    }
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, PBCAP_MAGIC, 4)) {
        fprintf(stderr, "[PBCAP] replay: %s is not a capture file\n", path);
        fclose(f);
        return 1;
    }
    fread(&version, 4, 1, f);
    fread(&ram_size, 4, 1, f);
    fread(&contig_size, 4, 1, f);
    fread(&nv2a_size, 4, 1, f);
    fread(&prelude_n, 4, 1, f);
    fread(&frame_n, 4, 1, f);
    fread(&flip, 4, 1, f);
    fread(&contig_hi, 4, 1, f);
    if (version != PBCAP_VERSION) {
        fprintf(stderr, "[PBCAP] replay: capture version %u, want %u\n",
                version, (unsigned)PBCAP_VERSION);
        fclose(f);
        return 1;
    }

    fprintf(stderr, "[PBCAP] replay: flip %u, %u prelude + %u frame methods\n",
            flip, prelude_n, frame_n);

    /* Stop the GPU poll so it cannot re-scan the restored DMA_PUT. */
    xbox_Nv2aAckStop();

    /* Restore the three snapshotted regions. */
    if (ram_size > PBCAP_RAM_SIZE || contig_size > PBCAP_CONTIG_SIZE
            || nv2a_size > PBCAP_NV2A_SIZE) {
        fprintf(stderr, "[PBCAP] replay: capture region too large\n");
        fclose(f);
        return 1;
    }
    blob = xrealloc(blob, PBCAP_RAM_SIZE);
    if (fread(blob, 1, ram_size, f) != ram_size) goto short_read;
    memcpy(base + PBCAP_RAM_BASE, blob, ram_size);
    if (fread(blob, 1, contig_size, f) != contig_size) goto short_read;
    memcpy(base + PBCAP_CONTIG_BASE, blob, contig_size);
    if (fread(blob, 1, nv2a_size, f) != nv2a_size) goto short_read;
    memcpy(base + PBCAP_NV2A_BASE, blob, nv2a_size);

    /* dma_resolve maps a physical offset against the contiguous high-water
     * mark; restore it so the frame's offsets resolve to the same bytes. */
    xbox_ContiguousSetAllocatedBytes(contig_hi);

    prelude = xrealloc(prelude, (size_t)(prelude_n ? prelude_n : 1) * sizeof(PbCapMethod));
    frame   = xrealloc(frame,   (size_t)(frame_n   ? frame_n   : 1) * sizeof(PbCapMethod));
    if (fread(prelude, sizeof(PbCapMethod), prelude_n, f) != prelude_n) goto short_read;
    if (fread(frame,   sizeof(PbCapMethod), frame_n,   f) != frame_n)   goto short_read;
    fclose(f);

    for (i = 0; i < prelude_n; i++)
        nv2a_pb_exec_method(prelude[i].subch, prelude[i].method, prelude[i].param);
    for (i = 0; i < frame_n; i++)
        nv2a_pb_exec_method(frame[i].subch, frame[i].method, frame[i].param);
    nv2a_pb_exec_batch_end();

    snprintf(bmp, sizeof bmp, "%s.replay.bmp", path);
    nv2a_pb_exec_dump_surface(bmp);
    fprintf(stderr, "[PBCAP] replay done: %s\n", bmp);
    return 0;

short_read:
    fprintf(stderr, "[PBCAP] replay: short read of capture file\n");
    fclose(f);
    return 1;
}
