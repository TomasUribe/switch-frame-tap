/* proto.h - copy of ../../proto.h. Keep identical. */
#ifndef SWITCH_STREAM_PROTO_H
#define SWITCH_STREAM_PROTO_H

#include <stdint.h>

#define SW_MAGIC        0x31565753u   /* "SWV1" */
#define SW_PROTO_PORT   9899

enum {
    SW_PKT_HELLO = 1,
    SW_PKT_VIDEO = 2,
    SW_PKT_AUDIO = 3,
};

enum {
    SW_FLAG_KEYFRAME = 1 << 0,
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  type;
    uint8_t  flags;
    uint16_t _rsvd;
    uint32_t size;
    uint64_t ts;
} sw_hdr_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t audio_rate;
    uint32_t audio_channels;
} sw_hello_t;
#pragma pack(pop)

#endif
