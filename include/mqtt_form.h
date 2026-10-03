#pragma once
#include <stdbool.h>
#include <stddef.h>

/* The setup-mode MQTT entry point — pure, host-tested. No ESP-IDF
   includes, no NVS calls, no httpd: this module only turns a request
   body into a validated mqtt_form_result_t or a field-level error. Task
   4's two handlers (the `/mqtt` HTML form and the `mqtt-config` protocomm
   endpoint) own the device calls; they parse with one of the two
   functions below, apply the result through nvs_config_set_mqtt_* on
   success, and turn a non-OK status into a message on the form page via
   mqtt_form_error_str().

   ---- buffer sizes ----

   MQTT_FORM_URI_MAX/USER_MAX/PASS_MAX match the stack buffers
   mqtt_ha.c's mqtt_ha_window() reads NVS into
   (`char uri[128], user[64], pass[64]` at main/mqtt_ha.c:840) and that
   nvs_config_get_mqtt_uri/user/pass fill. They are declared independently
   here rather than shared by symbol with mqtt_ha.c, because this task's
   scope is the validator only — if a future change resizes one of those
   stack buffers, it must update the matching constant here too, or a
   value this module accepts could overflow NVS's actual column width.

   ---- empty URI: MQTT is a disable-able feature, not a required one ----

   An empty `uri` is a valid result, not a missing field: nvs_config.h
   already documents "Empty URI = MQTT disabled", and a WiFi-only device
   is a supported state (plan doc, "Entering setup mode" table). Both
   parse functions implement this the same way: a successful parse with
   uri[0] == '\0' clears user and pass to "" and sets keep_pass = false,
   regardless of what the request body said for those two fields. That
   is a deliberate overwrite, not a "don't care" — a caller must not be
   able to read stale decoded user/pass bytes out of *out and mistake
   them for something that survived. The one way to get a MISSING error
   for uri is to not send the field at all (no `uri=` pair in the
   urlencoded body, no "uri" key in the JSON object); an explicit empty
   value is always accepted. */

#define MQTT_FORM_URI_MAX 128
#define MQTT_FORM_USER_MAX 64
#define MQTT_FORM_PASS_MAX 64

/* The request body length cap enforced by BOTH parse entry points,
   before anything else runs (including, for JSON, before cJSON ever
   sees the bytes). One constant so the form page and the protocomm
   endpoint cannot be tuned to disagree about how large a body the
   device will even look at. */
#define MQTT_FORM_BODY_MAX 512

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char uri[MQTT_FORM_URI_MAX];
    char user[MQTT_FORM_USER_MAX];
    char pass[MQTT_FORM_PASS_MAX];
    /* True when the submitted password should NOT overwrite the stored
       one: the body omitted `pass`, or sent it empty. False means pass[]
       holds a literal new password (store it), or the URI was empty
       (MQTT disabled; pass[] is "" and there is nothing to keep). A
       literal password is written here for the caller to store — never
       logged, never echoed into a response. */
    bool keep_pass;
} mqtt_form_result_t;

/* Which field a non-OK mqtt_form_status_t names. NONE for a body-level
   failure (body too long, bad JSON at the top level) that is not any
   one field's fault. */
typedef enum {
    MQTT_FORM_FIELD_NONE = 0,
    MQTT_FORM_FIELD_URI,
    MQTT_FORM_FIELD_USER,
    MQTT_FORM_FIELD_PASS,
} mqtt_form_field_t;

typedef enum {
    MQTT_FORM_ERR_NONE = 0,           /* success; *out is the validated result */
    MQTT_FORM_ERR_MISSING,            /* uri field absent entirely */
    MQTT_FORM_ERR_TOO_LONG,           /* decoded value will not fit its buffer */
    MQTT_FORM_ERR_BAD_SCHEME,         /* uri is not mqtt://, not mqtts://, or has no host */
    MQTT_FORM_ERR_BAD_CHAR,           /* disallowed character in uri, user or pass */
    MQTT_FORM_ERR_MALFORMED_ENCODING, /* a %-escape that isn't two hex digits, or a decoded NUL */
    MQTT_FORM_ERR_BODY_TOO_LONG,      /* request body over MQTT_FORM_BODY_MAX */
    MQTT_FORM_ERR_DUPLICATE_FIELD,    /* a known field name appeared twice */
    MQTT_FORM_ERR_BAD_JSON,           /* unparseable JSON, not an object, or a non-string field value */
} mqtt_form_err_t;

typedef struct {
    mqtt_form_err_t err;
    mqtt_form_field_t field; /* MQTT_FORM_FIELD_NONE when err has no single field */
} mqtt_form_status_t;

/* Parses an `application/x-www-form-urlencoded` body: `uri`, `user` and
   `pass` fields, `&`-separated, `+` decoded to space and `%XX` to its
   byte. body need not be NUL-terminated — body_len is authoritative.
   Unknown field names are ignored; a known field name appearing twice is
   MQTT_FORM_ERR_DUPLICATE_FIELD. On success (err == MQTT_FORM_ERR_NONE),
   *out is fully populated (see the empty-URI note above) and every byte
   of *out that is not explicitly documented as meaningful (e.g. the tail
   of a short string's buffer) has been zero-initialised. On failure,
   *out's contents are unspecified — callers must check err before
   reading it. */
mqtt_form_status_t mqtt_form_parse_urlencoded(const char *body, size_t body_len, mqtt_form_result_t *out);

/* Parses a JSON object body with the same three keys, via the vendored
   cJSON. A present key whose value is not a JSON string is
   MQTT_FORM_ERR_BAD_JSON naming that field; a body that isn't a JSON
   object at all is MQTT_FORM_ERR_BAD_JSON with MQTT_FORM_FIELD_NONE.
   Same body_len / empty-URI / zero-on-failure contract as the
   urlencoded parser above. */
mqtt_form_status_t mqtt_form_parse_json(const char *body, size_t body_len, mqtt_form_result_t *out);

/* Short, human-readable text for a status's err, for the message task
   4's form page shows next to the field it names. Pure: no field name is
   included (the caller already has mqtt_form_field_t for that), and no
   submitted value is ever echoed back, so a rejected password can never
   appear in this string. */
const char *mqtt_form_error_str(mqtt_form_err_t err);

/* Escapes &, <, >, " and ' so `in` is safe to place inside a double-quoted
   HTML attribute value (`value="..."`), which is how task 4's form page
   prefills uri and user from NVS. Writes the escaped text plus a NUL
   into out (capacity out_len) and returns true, or, if the escaped text
   (including its NUL) would not fit, writes an empty string to out
   (when out_len >= 1) and returns false. in == NULL, out == NULL or
   out_len == 0 is treated as "will not fit": returns false, and writes
   "" only if there is room to. Never used on a password — the plan's
   security note is "credentials are never echoed back", and this
   function exists only for the two fields the form page prefills. */
bool mqtt_form_html_escape(const char *in, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
