/* transport.h - single outbound pipe, either USB bulk or one TCP client. */
#ifndef SWITCH_STREAM_TRANSPORT_H
#define SWITCH_STREAM_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum { TR_USB, TR_TCP } tr_mode_t;

/* mode string: "usb", "tcp", or "tcp:<port>". */
int  tr_init(const char *mode);
void tr_exit(void);

tr_mode_t tr_get_mode(void);

/* Block until a receiver is attached (TCP accept; USB returns immediately once).
 * Returns 0 on success. On return, a HELLO has been sent. */
int  tr_wait_client(void);

/* True while a receiver is believed to be attached. */
bool tr_connected(void);

/* Drop the current receiver (close TCP client). */
void tr_drop_client(void);

/* Frame one packet (header + payload) and write it atomically.
 * Returns 0 on success, <0 on error (receiver likely gone). */
int  tr_send(uint8_t type, uint8_t flags, uint64_t ts,
             const void *payload, uint32_t len);

/* Re-send the HELLO so a late-joining receiver can resync (call before keyframes). */
int  tr_send_hello(void);

#endif
