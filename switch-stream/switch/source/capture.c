#include "capture.h"

#include <switch.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * libnx exposes the grc:d reader as grcdRead() on recent releases and as
 * grcdTransfer() on older ones. Both have the same signature:
 *
 *   Result grcdRead(GrcStream stream, void *buffer, size_t size,
 *                   u32 *unk, u32 *data_size, u64 *timestamp);
 *
 * GrcStream_Audio = 0, GrcStream_Video = 1.
 *
 * If your libnx only has grcdTransfer(), build with -DGRCD_READ=grcdTransfer.
 * If it has neither, bind the IPC directly the way SysDVR does (cmd 1 on the
 * "grc:d" service, in/out buffer + 3 u64 out).
 * ------------------------------------------------------------------------ */
#ifndef GRCD_READ
#define GRCD_READ grcdRead
#endif

int cap_init(void)
{
    Result rc = grcdInitialize();
    return R_SUCCEEDED(rc) ? 0 : -1;
}

void cap_exit(void)
{
    grcdExit();
}

/* Scan Annex-B data for an SPS (type 7) or IDR (type 5) NAL. */
static bool contains_keyframe(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 4 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 &&
            ((p[i + 2] == 1) || (p[i + 2] == 0 && p[i + 3] == 1))) {
            size_t hdr = (p[i + 2] == 1) ? 3 : 4;
            if (i + hdr >= n) break;
            unsigned nal_type = p[i + hdr] & 0x1F;
            if (nal_type == 5 || nal_type == 7)
                return true;
            i += hdr; /* skip past this start code */
        }
    }
    return false;
}

int cap_read_video(void *buf, size_t cap, uint64_t *ts, bool *key)
{
    u32 unk = 0, sz = 0;
    u64 timestamp = 0;
    Result rc = GRCD_READ(GrcStream_Video, buf, cap, &unk, &sz, &timestamp);

    /* grc:d returns an error (e.g. 0x218 "not ready") whenever no title is
     * recording or the ring is momentarily empty. All of these are soft:
     * the caller just sleeps briefly and retries. */
    if (R_FAILED(rc) || sz == 0)
        return 0;

    *ts  = timestamp;
    *key = contains_keyframe((const uint8_t *)buf, sz);
    return (int)sz;
}

int cap_read_audio(void *buf, size_t cap, uint64_t *ts)
{
    u32 unk = 0, sz = 0;
    u64 timestamp = 0;
    Result rc = GRCD_READ(GrcStream_Audio, buf, cap, &unk, &sz, &timestamp);

    if (R_FAILED(rc) || sz == 0)
        return 0;

    *ts = timestamp;
    return (int)sz;
}
