# Stream 2c — nvs_config.c Module Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `nvs_config.c` using TDD — typed NVS accessors with first-boot defaults. All tests run on the host via the mock NVS HAL; no hardware required.

**Architecture:** `nvs_config.c` is a thin typed API over `hal_nvs_*()`. `nvs_config_init_defaults()` writes compile-time constants from `nvs_defaults.h` to NVS only for keys that are missing (idempotent). Every accessor falls back to the compile-time default on `ESP_ERR_NVS_NOT_FOUND`.

**Tech Stack:** C99, Unity test framework, PlatformIO native, mock HAL from Stream 1

**Prerequisite:** Stream 1 (`feature/test-harness`) merged to `integration`.

---

## Files

| Action | Path |
|---|---|
| Modify | `test/test_nvs_config/test_nvs_config.c` |
| Modify | `src/nvs_config.c` |

`include/nvs_config.h` and `include/nvs_defaults.h` are created in Stream 0 — do not modify their signatures.

---

## Task 1: Create branch

- [ ] **Step 1: Branch from integration**

```bash
git fetch origin
git checkout integration
git checkout -b feature/nvs-config
```

---

## Task 2: Write failing tests

**Files:** Replace `test/test_nvs_config/test_nvs_config.c`

- [ ] **Step 1: Write all tests**

```c
#include <unity.h>
#include <string.h>
#include <stdint.h>

/* Single-TU compilation */
#include "mock_hal_nvs.c"
#include "../../src/nvs_config.c"

void setUp(void)
{
    mock_nvs_reset(); /* start each test with blank NVS */
}

void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* nvs_config_init_defaults                                            */
/* ------------------------------------------------------------------ */

void test_init_defaults_writes_weekday_min(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, val);
}

void test_init_defaults_writes_weekend_min(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekend_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKEND_MIN, val);
}

void test_init_defaults_writes_holiday_min(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holiday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_HOLIDAY_MIN, val);
}

void test_init_defaults_writes_holiday_blob(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    char buf[2048];
    size_t len = sizeof(buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holidays(buf, &len));
    /* Blob must contain at least one YYYY-MM-DD date */
    TEST_ASSERT_TRUE(len >= 10);
    /* New Year's Day must be present */
    TEST_ASSERT_NOT_NULL(strstr(buf, "2026-01-01"));
}

void test_init_defaults_is_idempotent(void)
{
    /* Call twice — second call must not overwrite user-modified values */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    /* Modify a value */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekday_min(45));
    /* Call defaults again */
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_init_defaults());
    /* Value must NOT revert to default */
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(45, val);
}

/* ------------------------------------------------------------------ */
/* u16 round-trips                                                      */
/* ------------------------------------------------------------------ */

void test_weekday_min_round_trip(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekday_min(45));
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(45, val);
}

void test_weekend_min_round_trip(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_weekend_min(90));
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_weekend_min(&val));
    TEST_ASSERT_EQUAL_UINT16(90, val);
}

void test_holiday_min_round_trip(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_holiday_min(180));
    uint16_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holiday_min(&val));
    TEST_ASSERT_EQUAL_UINT16(180, val);
}

/* ------------------------------------------------------------------ */
/* String round-trips                                                   */
/* ------------------------------------------------------------------ */

void test_wifi_ssid_round_trip(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_wifi_ssid("MyHomeNetwork"));
    char buf[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_wifi_ssid(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("MyHomeNetwork", buf);
}

void test_wifi_pass_round_trip(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_wifi_pass("hunter2"));
    char buf[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_wifi_pass(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("hunter2", buf);
}

void test_wifi_ssid_max_length(void)
{
    /* SSID max is 32 bytes per 802.11 */
    char long_ssid[33];
    memset(long_ssid, 'A', 32);
    long_ssid[32] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_wifi_ssid(long_ssid));
    char buf[64] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_wifi_ssid(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(long_ssid, buf);
}

/* ------------------------------------------------------------------ */
/* Blob round-trip                                                       */
/* ------------------------------------------------------------------ */

void test_holidays_blob_round_trip(void)
{
    const char *holidays = "2026-01-01\n2026-07-04\n2026-12-25\n";
    size_t write_len = strlen(holidays);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_holidays(holidays, write_len));

    char buf[256] = {0};
    size_t read_len = sizeof(buf);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_holidays(buf, &read_len));
    TEST_ASSERT_EQUAL_UINT32(write_len, read_len);
    TEST_ASSERT_EQUAL_MEMORY(holidays, buf, write_len);
}

/* ------------------------------------------------------------------ */
/* Display contrast                                                      */
/* ------------------------------------------------------------------ */

void test_display_contrast_round_trip(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_set_display_contrast(2));
    uint8_t val = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_config_get_display_contrast(&val));
    TEST_ASSERT_EQUAL_UINT8(2, val);
}

void test_display_contrast_missing_returns_default(void)
{
    /* No write — should fall back to NVS_DEFAULT_CONTRAST */
    uint8_t val = 255; /* sentinel */
    nvs_config_get_display_contrast(&val);
    TEST_ASSERT_EQUAL_UINT8(NVS_DEFAULT_CONTRAST, val);
}

/* ------------------------------------------------------------------ */
/* Missing key fallback                                                  */
/* ------------------------------------------------------------------ */

void test_get_weekday_min_missing_returns_default(void)
{
    uint16_t val = 0;
    nvs_config_get_weekday_min(&val); /* no write beforehand */
    TEST_ASSERT_EQUAL_UINT16(NVS_DEFAULT_WEEKDAY_MIN, val);
}

/* ------------------------------------------------------------------ */
/* Runner                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_defaults_writes_weekday_min);
    RUN_TEST(test_init_defaults_writes_weekend_min);
    RUN_TEST(test_init_defaults_writes_holiday_min);
    RUN_TEST(test_init_defaults_writes_holiday_blob);
    RUN_TEST(test_init_defaults_is_idempotent);
    RUN_TEST(test_weekday_min_round_trip);
    RUN_TEST(test_weekend_min_round_trip);
    RUN_TEST(test_holiday_min_round_trip);
    RUN_TEST(test_wifi_ssid_round_trip);
    RUN_TEST(test_wifi_pass_round_trip);
    RUN_TEST(test_wifi_ssid_max_length);
    RUN_TEST(test_holidays_blob_round_trip);
    RUN_TEST(test_display_contrast_round_trip);
    RUN_TEST(test_display_contrast_missing_returns_default);
    RUN_TEST(test_get_weekday_min_missing_returns_default);
    return UNITY_END();
}
```

- [ ] **Step 2: Run tests — verify they FAIL**

```bash
pio test -e native -f test_nvs_config
```

Expected: many FAIL lines. Stubs return 0 without reading/writing anything.

- [ ] **Step 3: Commit failing tests**

```bash
git add test/test_nvs_config/test_nvs_config.c
git commit -m "test(nvs_config): add full round-trip and defaults test suite (all failing)"
```

---

## Task 3: Implement u16 accessors

**Files:** Replace the full content of `src/nvs_config.c`

- [ ] **Step 1: Implement u16 read/write helpers and weekday/weekend/holiday accessors**

```c
#include "nvs_config.h"
#include "nvs_defaults.h"
#include "hal_nvs.h"
#include <string.h>

/* ---- u16 helpers ---- */

static esp_err_t get_u16_with_default(const char *key, uint16_t *out, uint16_t default_val)
{
    esp_err_t ret = hal_nvs_read_u16(key, out);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *out = default_val;
        return ESP_OK;
    }
    return ret;
}

static esp_err_t init_u16_if_missing(const char *key, uint16_t default_val)
{
    uint16_t tmp;
    esp_err_t ret = hal_nvs_read_u16(key, &tmp);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return hal_nvs_write_u16(key, default_val);
    }
    return ESP_OK; /* already set — do not overwrite */
}

/* ---- u16 accessors ---- */

esp_err_t nvs_config_get_weekday_min(uint16_t *out)
{
    return get_u16_with_default("weekday_min", out, NVS_DEFAULT_WEEKDAY_MIN);
}

esp_err_t nvs_config_set_weekday_min(uint16_t val)
{
    return hal_nvs_write_u16("weekday_min", val);
}

esp_err_t nvs_config_get_weekend_min(uint16_t *out)
{
    return get_u16_with_default("weekend_min", out, NVS_DEFAULT_WEEKEND_MIN);
}

esp_err_t nvs_config_set_weekend_min(uint16_t val)
{
    return hal_nvs_write_u16("weekend_min", val);
}

esp_err_t nvs_config_get_holiday_min(uint16_t *out)
{
    return get_u16_with_default("holiday_min", out, NVS_DEFAULT_HOLIDAY_MIN);
}

esp_err_t nvs_config_set_holiday_min(uint16_t val)
{
    return hal_nvs_write_u16("holiday_min", val);
}
```

- [ ] **Step 2: Run u16 round-trip tests**

```bash
pio test -e native -f test_nvs_config
```

Expected: all `test_*_min_round_trip` and `test_get_weekday_min_missing_returns_default` PASS.

---

## Task 4: Implement string accessors

- [ ] **Step 1: Add string accessors to src/nvs_config.c**

```c
esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len)
{
    esp_err_t ret = hal_nvs_read_str("wifi_ssid", buf, len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        return ESP_OK;
    }
    return ret;
}

esp_err_t nvs_config_set_wifi_ssid(const char *ssid)
{
    return hal_nvs_write_str("wifi_ssid", ssid);
}

esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len)
{
    esp_err_t ret = hal_nvs_read_str("wifi_pass", buf, len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        buf[0] = '\0';
        return ESP_OK;
    }
    return ret;
}

esp_err_t nvs_config_set_wifi_pass(const char *pass)
{
    return hal_nvs_write_str("wifi_pass", pass);
}
```

- [ ] **Step 2: Run string tests**

```bash
pio test -e native -f test_nvs_config
```

Expected: all `test_wifi_*` tests PASS.

---

## Task 5: Implement blob and contrast accessors

- [ ] **Step 1: Add blob and contrast accessors to src/nvs_config.c**

```c
esp_err_t nvs_config_get_holidays(char *buf, size_t *len)
{
    return hal_nvs_read_blob("holidays", buf, len);
}

esp_err_t nvs_config_set_holidays(const char *blob, size_t len)
{
    return hal_nvs_write_blob("holidays", blob, len);
}

esp_err_t nvs_config_get_display_contrast(uint8_t *out)
{
    uint16_t tmp;
    esp_err_t ret = get_u16_with_default("disp_contrast",
                                         &tmp, NVS_DEFAULT_CONTRAST);
    *out = (uint8_t)tmp;
    return ret;
}

esp_err_t nvs_config_set_display_contrast(uint8_t val)
{
    return hal_nvs_write_u16("disp_contrast", (uint16_t)val);
}
```

- [ ] **Step 2: Run blob and contrast tests**

```bash
pio test -e native -f test_nvs_config
```

Expected: `test_holidays_blob_round_trip`, `test_display_contrast_*` PASS.

---

## Task 6: Implement nvs_config_init_defaults

- [ ] **Step 1: Add init_defaults to src/nvs_config.c**

```c
esp_err_t nvs_config_init_defaults(void)
{
    esp_err_t ret;

    ret = init_u16_if_missing("weekday_min", NVS_DEFAULT_WEEKDAY_MIN);
    if (ret != ESP_OK) return ret;

    ret = init_u16_if_missing("weekend_min", NVS_DEFAULT_WEEKEND_MIN);
    if (ret != ESP_OK) return ret;

    ret = init_u16_if_missing("holiday_min", NVS_DEFAULT_HOLIDAY_MIN);
    if (ret != ESP_OK) return ret;

    ret = init_u16_if_missing("disp_contrast", NVS_DEFAULT_CONTRAST);
    if (ret != ESP_OK) return ret;

    /* Strings: write only if missing */
    char tmp[64];
    if (hal_nvs_read_str("wifi_ssid", tmp, sizeof(tmp)) == ESP_ERR_NVS_NOT_FOUND) {
        ret = hal_nvs_write_str("wifi_ssid", NVS_DEFAULT_WIFI_SSID);
        if (ret != ESP_OK) return ret;
    }
    if (hal_nvs_read_str("wifi_pass", tmp, sizeof(tmp)) == ESP_ERR_NVS_NOT_FOUND) {
        ret = hal_nvs_write_str("wifi_pass", NVS_DEFAULT_WIFI_PASS);
        if (ret != ESP_OK) return ret;
    }

    /* Holiday blob: write only if missing */
    {
        uint8_t blob_check[1];
        size_t blob_len = sizeof(blob_check);
        if (hal_nvs_read_blob("holidays", blob_check, &blob_len) == ESP_ERR_NVS_NOT_FOUND) {
            const char *defaults = NVS_DEFAULT_HOLIDAYS;
            size_t def_len = strlen(defaults);
            ret = hal_nvs_write_blob("holidays", defaults, def_len);
            if (ret != ESP_OK) return ret;
        }
    }

    return ESP_OK;
}
```

- [ ] **Step 2: Run ALL tests — verify all pass**

```bash
pio test -e native -f test_nvs_config
```

Expected:
```
-----------------------
15 Tests 0 Failures 0 Ignored
OK
================================= [PASSED] Took X.XXX seconds =================================
```

- [ ] **Step 3: Commit**

```bash
git add src/nvs_config.c test/test_nvs_config/test_nvs_config.c
git commit -m "feat(nvs_config): implement typed NVS accessors and first-boot defaults"
```

---

## Task 7: Verify magtag build compiles

- [ ] **Step 1: Run pio run**

```bash
pio run
```

Expected: `[SUCCESS]`. If `nvs_flash.h` or `nvs.h` not found, check `src/CMakeLists.txt` has `nvs_flash` in `REQUIRES`.

---

## Task 8: Code review

- [ ] **Step 1: Run adversarial code review**

Use `superpowers:requesting-code-review` skill. Focus areas:
- `init_defaults` idempotency: the blob-check reads 1 byte into a 1-byte buffer — what if `read_blob` partially fills? (The return code `ESP_ERR_NVS_NOT_FOUND` is the distinguishing check, not the data)
- `get_u16_with_default`: what if `hal_nvs_read_u16` fails with an error other than `NOT_FOUND`? (Returns that error — is silent fallback to default more appropriate here? Discuss in review.)
- NVS key names: all within 15-char NVS limit? (`disp_contrast` = 13 chars ✓, `weekday_min` = 11 ✓)
- `display_contrast` stored as `uint16_t` but typed as `uint8_t` in the API — is the cast safe? (Values 0–2; yes)
- Holiday blob: `NVS_DEFAULT_HOLIDAYS` is a string literal; `strlen()` doesn't count the trailing null. Blob is stored without null terminator — correct for binary blobs

Fix any issues before merging.

---

## Task 9: Merge to integration

- [ ] **Step 1: Push and merge**

```bash
git push -u origin feature/nvs-config
git checkout integration
git merge --no-ff feature/nvs-config -m "feat(nvs_config): typed NVS accessors and first-boot defaults with TDD"
git push origin integration
```
