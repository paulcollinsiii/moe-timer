#include "mqtt_rx.h"

#include <stdbool.h>
#include <string.h>

/* The delivered topic is NOT NUL-terminated — esp-mqtt hands over a pointer
   into its receive buffer plus a length — so the comparison is length-then-
   memcmp rather than strcmp.

   An unconfigured route (a zeroed mqtt_rx_t, or one whose topic was never
   filled in) leaves `want` NULL, and this makes such a route inert. The
   `want != NULL` leg is not a tie-breaker against over-matching: without
   it the very next operand is strlen(want), so the route would not
   over-match, it would dereference NULL and crash. Short-circuit order is
   therefore part of the contract, not incidental. */
static bool topic_is(const char *topic, int topic_len, const char *want) {
    return want != NULL && topic_len >= 0 && (size_t)topic_len == strlen(want) &&
           memcmp(topic, want, (size_t)topic_len) == 0;
}

/* True when the chunk [offset, offset + data_len) lies inside the payload
   the broker declared, i.e. when the memcpy that follows cannot run past
   the end of a buffer already checked against total_len.

   Correct MQTT framing makes this always true — esp-mqtt derives both
   numbers from the same remaining-length header — so this is not a case
   that is expected to fire. It is here so the copies are safe BY
   INSPECTION rather than safe by trusting the broker and the client
   library, and it is checked only after a branch has claimed the topic, so
   a malformed frame on someone else's topic stays a silent ignore.

   Written as a subtraction, not `offset + data_len <= total_len`: with two
   attacker-influenced ints that sum can overflow, which is undefined
   behaviour and (on wrap) would pass the very check it is written to make.
   Both operands are already known non-negative here, so the difference
   cannot overflow. */
static bool chunk_within_payload(int offset, int data_len, int total_len) {
    return offset >= 0 && data_len >= 0 && offset <= total_len - data_len;
}

mqtt_rx_result_t mqtt_rx_on_data(mqtt_rx_t *rx, const char *topic, int topic_len, const char *data, int data_len,
                                 int total_len, int offset) {
    /* An empty retained payload (topic cleared) is ignored. Both pointers
       are checked, not just the lengths, because esp-mqtt nulls each of
       them independently: `data` whenever a fragment carries no bytes, and
       `topic` on every fragment of a split message after the first (see
       the buffer.size note in mqtt_ha.c — it is why a fragmented document
       cannot be reassembled here at all). A NULL topic has nothing to
       route by, and memcpy from NULL is undefined even for a zero count. */
    if (topic == NULL || data == NULL || total_len <= 0)
        return MQTT_RX_IGNORED;
    /* set/+ first — content check, not length (set/tz vs config collide). */
    if (topic_len > rx->set_prefix_len && rx->set_prefix_len >= 5 && offset == 0 && strncmp(topic, "magtag/", 7) == 0 &&
        strncmp(topic + rx->set_prefix_len - 5, "/set/", 5) == 0) {
        /* magtag/<id>/set/<key>: buffer the field key + value (small,
           single-chunk). The capacity test below is `data_len >=
           sizeof(value)`, which a NEGATIVE data_len passes — and it then
           reaches memcpy cast to size_t. Same hazard as the two document
           branches, so the same bound runs first. */
        if (!chunk_within_payload(offset, data_len, total_len))
            return MQTT_RX_BAD_CHUNK;
        if (rx->set_count >= rx->sets_cap)
            return MQTT_RX_SETS_FULL;
        int klen = topic_len - rx->set_prefix_len; /* > 0 per the outer check */
        if (klen >= (int)sizeof(rx->sets[0].key) || data_len >= (int)sizeof(rx->sets[0].value))
            return MQTT_RX_SET_TOO_LONG;
        memcpy(rx->sets[rx->set_count].key, topic + rx->set_prefix_len, (size_t)klen);
        rx->sets[rx->set_count].key[klen] = '\0';
        memcpy(rx->sets[rx->set_count].value, data, (size_t)data_len);
        rx->sets[rx->set_count].value[data_len] = '\0';
        rx->set_count++;
        return MQTT_RX_OK;
    }
    /* Chunked payloads (data_len < total_len) are copied by absolute offset.
       Capacity is judged before framing: "your document is too big" is a
       statement about the whole document and is what the operator needs to
       hear, whereas a bad chunk is a statement about one frame. */
    if (topic_is(topic, topic_len, rx->config_topic)) {
        if (total_len >= rx->config_cap) {
            /* Refused, NOT ignored. These used to share MQTT_RX_IGNORED
               with "not my topic", so a retained over-capacity document was
               re-delivered and re-dropped on every single reconnect with
               nothing logged and no ack published. The flags are read from
               the network task, which is the one that can publish.

               PAYLOAD BEFORE FLAG, and the order is load-bearing rather
               than stylistic. This function runs on the mqtt client task;
               apply_incoming() on the net_win task spins until the flag is
               set and then reads the length (mqtt_ha.c). Setting the flag
               first lets a preemption between the two stores publish a
               RETAINED ack reading "len":0 — losing the one fact that ack
               exists to carry, on the topic where the last write sticks.
               Same shape as the completion path below, which terminates
               config_buf before it sets config_done. */
            rx->config_too_long_len = total_len;
            rx->config_too_long = true;
            return MQTT_RX_CONFIG_TOO_LONG;
        }
        if (!chunk_within_payload(offset, data_len, total_len))
            return MQTT_RX_BAD_CHUNK;
        memcpy(rx->config_buf + offset, data, (size_t)data_len);
        if (offset + data_len >= total_len) {
            rx->config_buf[total_len] = '\0';
            rx->config_done = true;
        }
        return MQTT_RX_OK;
    }
    if (topic_is(topic, topic_len, rx->cmd_topic)) {
        if (total_len >= rx->cmd_cap) {
            /* Payload before flag, for the reason spelled out above. */
            rx->cmd_too_long_len = total_len;
            rx->cmd_too_long = true;
            return MQTT_RX_CMD_TOO_LONG;
        }
        if (!chunk_within_payload(offset, data_len, total_len))
            return MQTT_RX_BAD_CHUNK;
        memcpy(rx->cmd_buf + offset, data, (size_t)data_len);
        if (offset + data_len >= total_len) {
            rx->cmd_buf[total_len] = '\0';
            rx->cmd_done = true;
        }
        return MQTT_RX_OK;
    }
    return MQTT_RX_IGNORED;
}
