#include "mock_hal_nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 64
#define MAX_KEY_LEN 16
#define MAX_VAL_SIZE 2048

typedef struct {
    char key[MAX_KEY_LEN];
    uint8_t data[MAX_VAL_SIZE];
    size_t len;
    int used;
} Entry;

static Entry s_store[MAX_ENTRIES];
static int s_fail_writes;

/* Per-key call accounting, kept separate from the store so misses count
   too — a read of an absent key is still a flash access. */
typedef struct {
    char key[MAX_KEY_LEN];
    int calls;
} CallCount;

static CallCount s_read_counts[MAX_ENTRIES];
static CallCount s_write_counts[MAX_ENTRIES];

static void count_call(CallCount *table, const char *key) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (table[i].calls > 0 && strcmp(table[i].key, key) == 0) {
            table[i].calls++;
            return;
        }
    }
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (table[i].calls == 0) {
            strncpy(table[i].key, key, MAX_KEY_LEN - 1);
            table[i].key[MAX_KEY_LEN - 1] = '\0';
            table[i].calls = 1;
            return;
        }
    }
}

static int lookup_calls(const CallCount *table, const char *key) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (table[i].calls > 0 && strcmp(table[i].key, key) == 0) {
            return table[i].calls;
        }
    }
    return 0;
}

static void count_read(const char *key) {
    count_call(s_read_counts, key);
}

/* Counted before the failure injection is consumed: an attempted write is
   an attempted flash access whether or not it lands. */
static void count_write(const char *key) {
    count_call(s_write_counts, key);
}

int mock_nvs_read_count(const char *key) {
    return lookup_calls(s_read_counts, key);
}

int mock_nvs_write_count(const char *key) {
    return lookup_calls(s_write_counts, key);
}

void mock_nvs_reset(void) {
    memset(s_store, 0, sizeof(s_store));
    memset(s_read_counts, 0, sizeof(s_read_counts));
    memset(s_write_counts, 0, sizeof(s_write_counts));
    s_fail_writes = 0;
}

void mock_nvs_fail_writes(int count) {
    s_fail_writes = count;
}

/* Consume one injected failure; true = this write must return ESP_FAIL. */
static int take_write_failure(void) {
    if (s_fail_writes == 0)
        return 0;
    if (s_fail_writes > 0)
        s_fail_writes--;
    return 1;
}

static Entry *find_entry(const char *key) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (s_store[i].used && strcmp(s_store[i].key, key) == 0) {
            return &s_store[i];
        }
    }
    return NULL;
}

static Entry *alloc_entry(const char *key) {
    if (strlen(key) >= MAX_KEY_LEN)
        return NULL; /* key too long — matches ESP-IDF behavior */
    Entry *e = find_entry(key);
    if (e)
        return e;
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (!s_store[i].used) {
            s_store[i].used = 1;
            strncpy(s_store[i].key, key, MAX_KEY_LEN - 1);
            s_store[i].key[MAX_KEY_LEN - 1] = '\0';
            return &s_store[i];
        }
    }
    return NULL; /* store full */
}

esp_err_t hal_nvs_read_u16(const char *key, uint16_t *out) {
    count_read(key);
    const Entry *e = find_entry(key);
    if (!e || e->len != sizeof(uint16_t))
        return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, e->data, sizeof(uint16_t));
    return ESP_OK;
}

esp_err_t hal_nvs_write_u16(const char *key, uint16_t val) {
    count_write(key);
    if (take_write_failure())
        return ESP_FAIL;
    Entry *e = alloc_entry(key);
    if (!e)
        return ESP_FAIL;
    memcpy(e->data, &val, sizeof(uint16_t));
    e->len = sizeof(uint16_t);
    return ESP_OK;
}

esp_err_t hal_nvs_read_str(const char *key, char *buf, size_t *len) {
    count_read(key);
    const Entry *e = find_entry(key);
    if (!e)
        return ESP_ERR_NVS_NOT_FOUND;
    if (buf == NULL || *len == 0) {
        *len = e->len + 1; /* report required size including NUL */
        return ESP_OK;
    }
    size_t copy = (e->len < *len) ? e->len : *len - 1;
    memcpy(buf, e->data, copy);
    buf[copy] = '\0';
    *len = copy + 1; /* match ESP-IDF: *len includes NUL byte */
    return ESP_OK;
}

esp_err_t hal_nvs_write_str(const char *key, const char *val) {
    count_write(key);
    if (take_write_failure())
        return ESP_FAIL;
    Entry *e = alloc_entry(key);
    if (!e)
        return ESP_FAIL;
    e->len = strlen(val);
    if (e->len >= MAX_VAL_SIZE)
        e->len = MAX_VAL_SIZE - 1;
    memcpy(e->data, val, e->len);
    e->data[e->len] = '\0';
    return ESP_OK;
}

esp_err_t hal_nvs_read_blob(const char *key, void *buf, size_t *len) {
    count_read(key);
    const Entry *e = find_entry(key);
    if (!e)
        return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) {
        *len = e->len;
        return ESP_FAIL;
    }
    memcpy(buf, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t hal_nvs_write_blob(const char *key, const void *buf, size_t len) {
    count_write(key);
    if (take_write_failure())
        return ESP_FAIL;
    Entry *e = alloc_entry(key);
    if (!e)
        return ESP_FAIL;
    if (len > MAX_VAL_SIZE)
        return ESP_FAIL;
    memcpy(e->data, buf, len);
    e->len = len;
    return ESP_OK;
}

void hal_nvs_close(void) {
    /* Real impl caches the NVS handle across a wake; nothing to do here. */
}
