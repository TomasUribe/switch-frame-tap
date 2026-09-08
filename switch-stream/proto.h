/* proto.h - wire protocol shared by the Switch sender and the PC receiver.
 *
 * One TCP connection (or one USB bulk pipe) carries an interleaved stream of
 * length-prefixed packets. Every packet is: sw_hdr_t, then `size` payload bytes.
 * Multibyte fields are little-endian (both ARM Switch and x86 PC are LE).
 */
#ifndef SWITCH_STREAM_PROTO_H
#define SWITCH_STREAM_PROTO_H

#include <stdint.h>

#define SW_MAGIC        0x31565753u   /* "SWV1" */
#define SW_PROTO_PORT   9899          /* default TCP port */

enum {
    SW_PKT_HELLO = 1,  /* sender -> receiver, once per connection: sw_hello_t     */
    SW_PKT_VIDEO = 2,  /* H.264 Annex-B, 1+ complete NAL units, 00 00 00 01 start */
    SW_PKT_AUDIO = 3,  /* PCM S16LE, interleaved stereo, 48000 Hz                 */
};

enum {
    SW_FLAG_KEYFRAME = 1 << 0,  /* video packet contains SPS/PPS/IDR              */
};

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;   /* SW_MAGIC */
    uint8_t  type;    /* SW_PKT_* */
    uint8_t  flags;   /* SW_FLAG_* */
    uint16_t _rsvd;
    uint32_t size;    /* payload byte count immediately following this header */
    uint64_t ts;      /* source timestamp in nanoseconds, as reported by grc:d */
} sw_hdr_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t audio_rate;      /* 48000 */
    uint32_t audio_channels;  /* 2 */
} sw_hello_t;
#pragma pack(pop)

#endif /* SWITCH_STREAM_PROTO_H */
