#include "mqtt_rx.h"

#include <string.h>

mqtt_rx_result_t mqtt_rx_on_data(mqtt_rx_t *rx, const char *topic, int topic_len, const char *data, int data_len,
                                 int total_len, int offset) {
    /* An empty retained payload (topic cleared) is ignored. */
    if (topic == NULL || total_len <= 0)
        return MQTT_RX_IGNORED;
    /* set/+ first — content check, not length (set/tz vs config collide). */
    if (topic_len > rx->set_prefix_len && rx->set_prefix_len >= 5 && offset == 0 && strncmp(topic, "magtag/", 7) == 0 &&
        strncmp(topic + rx->set_prefix_len - 5, "/set/", 5) == 0) {
        /* magtag/<id>/set/<key>: buffer the field key + value (small,
           single-chunk). */
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
    /* Chunked payloads (data_len < total_len) are copied by absolute offset. */
    if (topic_len == rx->config_topic_len && total_len < rx->config_cap) {
        memcpy(rx->config_buf + offset, data, (size_t)data_len);
        if (offset + data_len >= total_len) {
            rx->config_buf[total_len] = '\0';
            rx->config_done = true;
        }
        return MQTT_RX_OK;
    }
    if (topic_len == rx->cmd_topic_len && total_len < rx->cmd_cap) {
        memcpy(rx->cmd_buf + offset, data, (size_t)data_len);
        if (offset + data_len >= total_len) {
            rx->cmd_buf[total_len] = '\0';
            rx->cmd_done = true;
        }
        return MQTT_RX_OK;
    }
    return MQTT_RX_IGNORED;
}
