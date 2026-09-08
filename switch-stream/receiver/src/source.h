/* source.h - packet source: one TCP connection or one USB bulk pipe. */
#ifndef SWITCH_STREAM_SOURCE_H
#define SWITCH_STREAM_SOURCE_H

#include <stddef.h>
#include <stdint.h>
#include "proto.h"

typedef struct source source_t;

source_t *source_open_tcp(const char *host, int port);   /* NULL on failure */
source_t *source_open_usb(void);

/* Read exactly one packet.
 *   returns  1  = packet delivered (hdr + payload in buf)
 *            0  = no data yet (timeout) - call again
 *           -1  = fatal (disconnected)
 * `cap` must be large enough for the largest expected payload. */
int  source_read(source_t *s, sw_hdr_t *hdr, uint8_t *buf, size_t cap);

void source_close(source_t *s);

#endif
