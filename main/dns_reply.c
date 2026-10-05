/* The captive portal's DNS reply builder — see include/dns_reply.h. */
#include "dns_reply.h"

#include <string.h>

#define DNS_HEADER_LEN 12
#define DNS_NAME_MAX 255
#define DNS_LABEL_MAX 63
#define DNS_A_RR_LEN 16
#define DNS_TTL_SEC 60 /* short, so a phone that leaves does not carry the answer to another network */

/* Walks the one question's name; returns its wire length (labels plus the
   root byte), or 0 if it is truncated, compressed or over the limits. */
static size_t question_name_len(const uint8_t *q, size_t qlen) {
    size_t o = DNS_HEADER_LEN;
    for (;;) {
        if (o >= qlen)
            return 0;
        uint8_t len = q[o];
        if (len == 0) {
            o++; /* the root byte counts toward the 255 too */
            if (o - DNS_HEADER_LEN > DNS_NAME_MAX)
                return 0;
            break;
        }
        /* The top two bits flag a compression pointer (11) or a reserved
           form; a question has nothing earlier to point at, so any length
           over 63 is junk either way. */
        if (len > DNS_LABEL_MAX)
            return 0;
        o += 1u + len;
    }
    return o - DNS_HEADER_LEN;
}

size_t dns_reply_build(const uint8_t *query, size_t qlen, const uint8_t ip[4], uint8_t *out, size_t out_cap) {
    if (query == NULL || out == NULL || ip == NULL || qlen < DNS_HEADER_LEN || qlen > DNS_QUERY_MAX)
        return 0;

    uint8_t flags_hi = query[2];
    if ((flags_hi & 0x80) != 0 || ((flags_hi >> 3) & 0x0F) != 0) /* a response, or not a standard query */
        return 0;
    if (query[4] != 0 || query[5] != 1) /* QDCOUNT must be exactly 1 */
        return 0;

    size_t name_len = question_name_len(query, qlen);
    if (name_len == 0)
        return 0;
    size_t question_len = name_len + 4; /* QTYPE + QCLASS */
    if (DNS_HEADER_LEN + question_len > qlen)
        return 0;

    const uint8_t *qtail = query + DNS_HEADER_LEN + name_len;
    uint16_t qtype = (uint16_t)((qtail[0] << 8) | qtail[1]);
    uint16_t qclass = (uint16_t)((qtail[2] << 8) | qtail[3]);
    int answer = (qtype == 1 && qclass == 1);

    size_t reply_len = DNS_HEADER_LEN + question_len + (answer ? DNS_A_RR_LEN : 0);
    if (reply_len > out_cap)
        return 0;

    memcpy(out, query, DNS_HEADER_LEN + question_len);
    out[2] = (uint8_t)(0x80 | (flags_hi & 0x01)); /* QR, opcode 0, AA 0, TC 0, RD echoed */
    out[3] = 0x80;                                /* RA, rcode NOERROR */
    out[6] = 0;                                   /* ANCOUNT */
    out[7] = (uint8_t)(answer ? 1 : 0);
    out[8] = out[9] = 0;   /* NSCOUNT */
    out[10] = out[11] = 0; /* ARCOUNT: the query's OPT record is not echoed */

    if (answer) {
        uint8_t *rr = out + DNS_HEADER_LEN + question_len;
        const uint8_t head[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, DNS_TTL_SEC, 0, 4};
        memcpy(rr, head, sizeof(head));
        memcpy(rr + sizeof(head), ip, 4);
    }
    return reply_len;
}
