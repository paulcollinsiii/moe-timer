#pragma once
#include <stddef.h>
#include <stdint.h>

/* The captive portal's DNS answer, pure: bytes in, bytes out, no sockets.
   setup_session_idf.c owns the UDP socket and the task; this is only the
   part worth a host test, because a wrong byte here fails silently on a
   phone (no popup, no error).

   Every A query gets one address (the SoftAP's own), so a phone's
   connectivity probe lands on our httpd instead of the internet. Anything
   else is answered with an empty NOERROR rather than NXDOMAIN: phones ask
   for AAAA and HTTPS records beside every A, and an NXDOMAIN there can
   make a resolver stop trying the A as well. */

/* The largest query this accepts: classic UDP DNS, before EDNS. */
#define DNS_QUERY_MAX 512

/* The largest reply it can build: header, a 255-byte name plus QTYPE/QCLASS,
   and one 16-byte A record. */
#define DNS_REPLY_MAX (12 + 255 + 4 + 16)

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the reply to `query` (qlen bytes) into `out` (out_cap bytes) and
   returns its length, or 0 to send nothing: a packet shorter than a header,
   a response or a non-query opcode, anything but exactly one question, a
   name that is truncated, compressed, has a label over 63 bytes or runs
   over 255 bytes, or an `out` too small for the reply. Silence is the
   right answer to all of these; replying to junk is how a responder
   becomes a reflector.

   Only the question section is echoed, so an EDNS OPT record in the
   query is not carried into the reply. `ip` is the 4 address bytes in
   network order. */
size_t dns_reply_build(const uint8_t *query, size_t qlen, const uint8_t ip[4], uint8_t *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
