/* Setup-mode MQTT entry point — see include/mqtt_form.h for the module's
   job, the empty-URI contract and the failure contract. Two parse entry
   points funnel into one `finalize()` so the form page and the JSON
   endpoint can never accept a value the other would refuse, and every
   non-OK return — from either entry point, or from finalize() itself —
   goes through fail() below so *out is always zeroed on the way out. */
#include "mqtt_form.h"

#include <string.h>

#include "cJSON.h"
#include "config_validate.h" /* config_mqtt_uri_check */

/* ---- the one non-OK exit, shared by every failure path below -------- */

static mqtt_form_status_t fail(mqtt_form_result_t *out, mqtt_form_err_t err, mqtt_form_field_t field) {
    memset(out, 0, sizeof(*out));
    return (mqtt_form_status_t){err, field};
}

/* ---- shared field-value checks ------------------------------------- */

/* True if s contains a C0 control character (< 0x20) or DEL (0x7F).
   Deliberately not config_is_clean_str: that rule also forbids '"' and
   '\\', which this field's spec does not ask for (a username or
   password containing either is unusual but not a device-level hazard —
   these values are never interpolated into hand-built JSON the way a
   config field is). */
static bool has_control_char(const char *s) {
    for (; *s != '\0'; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7F)
            return true;
    return false;
}

/* The semantic rules, applied identically after either parser has
   populated out's three buffers with raw (decoded) field content. */
static mqtt_form_status_t finalize(mqtt_form_result_t *out) {
    if (out->uri[0] == '\0') {
        /* Empty URI = MQTT disabled. user/pass are meaningless in this
           state; clear them so a caller cannot mistake decoded bytes
           left over from the request for something it should keep. */
        out->user[0] = '\0';
        out->pass[0] = '\0';
        out->keep_pass = false;
        return (mqtt_form_status_t){MQTT_FORM_ERR_NONE, MQTT_FORM_FIELD_NONE};
    }

    config_mqtt_uri_check_t uri_check = config_mqtt_uri_check(out->uri);
    if (uri_check != CONFIG_MQTT_URI_OK) {
        /* NO_HOST reads to the person filling in the form as "you didn't
           finish the scheme", the same as BAD_SCHEME; every other
           non-OK reason is a problem with a character somewhere after
           the scheme (a bad port, userinfo, a path, ...). */
        mqtt_form_err_t err = (uri_check == CONFIG_MQTT_URI_BAD_SCHEME || uri_check == CONFIG_MQTT_URI_NO_HOST)
                                  ? MQTT_FORM_ERR_BAD_SCHEME
                                  : MQTT_FORM_ERR_BAD_CHAR;
        return fail(out, err, MQTT_FORM_FIELD_URI);
    }

    if (has_control_char(out->user))
        return fail(out, MQTT_FORM_ERR_BAD_CHAR, MQTT_FORM_FIELD_USER);

    if (out->pass[0] == '\0') {
        out->keep_pass = true;
    } else {
        if (has_control_char(out->pass))
            return fail(out, MQTT_FORM_ERR_BAD_CHAR, MQTT_FORM_FIELD_PASS);
        out->keep_pass = false;
    }

    return (mqtt_form_status_t){MQTT_FORM_ERR_NONE, MQTT_FORM_FIELD_NONE};
}

/* ---- application/x-www-form-urlencoded ------------------------------ */

static int hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decodes one urlencoded value straight into dst (capacity dst_cap), so
   "too long" falls out of the same bounds check as the decode itself —
   no separate staging buffer, no separate length pass. '+' decodes to
   space. '%' must be followed by two hex digits or this is
   MQTT_FORM_ERR_MALFORMED_ENCODING ('%', '%4' and '%zz' all land here);
   a decoded NUL ('%00') is rejected the same way, since passing one
   through would silently truncate whatever reads dst as a C string. */
static mqtt_form_err_t decode_value(const char *src, size_t src_len, char *dst, size_t dst_cap) {
    size_t o = 0;
    for (size_t i = 0; i < src_len; i++) {
        char c = src[i];
        char decoded;
        if (c == '+') {
            decoded = ' ';
        } else if (c == '%') {
            if (i + 2 >= src_len)
                return MQTT_FORM_ERR_MALFORMED_ENCODING;
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi < 0 || lo < 0)
                return MQTT_FORM_ERR_MALFORMED_ENCODING;
            decoded = (char)((hi << 4) | lo);
            if (decoded == '\0')
                return MQTT_FORM_ERR_MALFORMED_ENCODING;
            i += 2;
        } else {
            decoded = c;
        }
        if (o + 1 >= dst_cap) /* +1 reserves room for the NUL below */
            return MQTT_FORM_ERR_TOO_LONG;
        dst[o++] = decoded;
    }
    dst[o] = '\0';
    return MQTT_FORM_ERR_NONE;
}

/* One form field the walker below fills: its key, where the decoded value
   goes, and whether the key has been seen (a second sighting is a
   DUPLICATE_FIELD). The two urlencoded entry points differ only in this
   table, so they cannot disagree about decoding. */
typedef struct {
    const char *name;
    char *dst;
    size_t dst_cap;
    mqtt_form_field_t field;
    bool seen;
} form_field_t;

/* Decodes every `key=value` pair of body into the matching entry of
   fields[0..nfields). Unknown keys are ignored. The caller owns zeroing
   its result on a non-OK return. */
static mqtt_form_status_t decode_pairs(const char *body, size_t body_len, form_field_t *fields, size_t nfields) {
    if (body_len > MQTT_FORM_BODY_MAX)
        return (mqtt_form_status_t){MQTT_FORM_ERR_BODY_TOO_LONG, MQTT_FORM_FIELD_NONE};
    /* A raw NUL byte would silently truncate whatever reads a field
       buffer as a C string later, the same hazard a decoded '%00' is —
       reject it here so every byte of body has actually been looked at,
       not just the bytes decode_value happens to reach before quitting. */
    if (memchr(body, '\0', body_len) != NULL)
        return (mqtt_form_status_t){MQTT_FORM_ERR_MALFORMED_ENCODING, MQTT_FORM_FIELD_NONE};

    size_t i = 0;
    while (i < body_len) {
        size_t pair_start = i;
        while (i < body_len && body[i] != '&')
            i++;
        size_t pair_end = i;
        if (i < body_len)
            i++; /* skip '&' */
        if (pair_end == pair_start)
            continue; /* "a=1&&b=2": the empty pair is just skipped */

        size_t eq = pair_start;
        while (eq < pair_end && body[eq] != '=')
            eq++;
        const char *key = body + pair_start;
        size_t key_len = eq - pair_start;
        const char *val = (eq < pair_end) ? body + eq + 1 : body + pair_end;
        size_t val_len = (eq < pair_end) ? (size_t)(pair_end - eq - 1) : 0;

        form_field_t *f = NULL;
        for (size_t k = 0; k < nfields; k++) {
            if (strlen(fields[k].name) == key_len && memcmp(key, fields[k].name, key_len) == 0) {
                f = &fields[k];
                break;
            }
        }
        if (f == NULL)
            continue; /* unknown field: ignored, per the module contract */

        if (f->seen)
            return (mqtt_form_status_t){MQTT_FORM_ERR_DUPLICATE_FIELD, f->field};
        f->seen = true;

        mqtt_form_err_t derr = decode_value(val, val_len, f->dst, f->dst_cap);
        if (derr != MQTT_FORM_ERR_NONE)
            return (mqtt_form_status_t){derr, f->field};
    }
    return (mqtt_form_status_t){MQTT_FORM_ERR_NONE, MQTT_FORM_FIELD_NONE};
}

mqtt_form_status_t mqtt_form_parse_urlencoded(const char *body, size_t body_len, mqtt_form_result_t *out) {
    memset(out, 0, sizeof(*out));
    form_field_t fields[] = {
        {"uri", out->uri, MQTT_FORM_URI_MAX, MQTT_FORM_FIELD_URI, false},
        {"user", out->user, MQTT_FORM_USER_MAX, MQTT_FORM_FIELD_USER, false},
        {"pass", out->pass, MQTT_FORM_PASS_MAX, MQTT_FORM_FIELD_PASS, false},
    };
    mqtt_form_status_t st = decode_pairs(body, body_len, fields, sizeof(fields) / sizeof(fields[0]));
    if (st.err != MQTT_FORM_ERR_NONE)
        return fail(out, st.err, st.field);

    if (!fields[0].seen)
        return fail(out, MQTT_FORM_ERR_MISSING, MQTT_FORM_FIELD_URI);

    return finalize(out);
}

static mqtt_form_status_t setup_fail(mqtt_form_setup_t *out, mqtt_form_err_t err, mqtt_form_field_t field) {
    memset(out, 0, sizeof(*out));
    return (mqtt_form_status_t){err, field};
}

mqtt_form_status_t mqtt_form_parse_setup(const char *body, size_t body_len, mqtt_form_setup_t *out) {
    memset(out, 0, sizeof(*out));
    form_field_t fields[] = {
        {"ssid", out->ssid, MQTT_FORM_WIFI_SSID_MAX, MQTT_FORM_FIELD_WIFI_SSID, false},
        {"wpass", out->wifi_pass, MQTT_FORM_WIFI_PASS_MAX, MQTT_FORM_FIELD_WIFI_PASS, false},
        {"uri", out->mqtt.uri, MQTT_FORM_URI_MAX, MQTT_FORM_FIELD_URI, false},
        {"user", out->mqtt.user, MQTT_FORM_USER_MAX, MQTT_FORM_FIELD_USER, false},
        {"pass", out->mqtt.pass, MQTT_FORM_PASS_MAX, MQTT_FORM_FIELD_PASS, false},
    };
    mqtt_form_status_t st = decode_pairs(body, body_len, fields, sizeof(fields) / sizeof(fields[0]));
    if (st.err != MQTT_FORM_ERR_NONE)
        return setup_fail(out, st.err, st.field);

    if (out->ssid[0] != '\0') {
        size_t pass_len = strlen(out->wifi_pass);
        if (has_control_char(out->ssid))
            return setup_fail(out, MQTT_FORM_ERR_BAD_CHAR, MQTT_FORM_FIELD_WIFI_SSID);
        if (has_control_char(out->wifi_pass))
            return setup_fail(out, MQTT_FORM_ERR_BAD_CHAR, MQTT_FORM_FIELD_WIFI_PASS);
        if (pass_len > 0 && pass_len < 8)
            return setup_fail(out, MQTT_FORM_ERR_TOO_SHORT, MQTT_FORM_FIELD_WIFI_PASS);
        /* A passphrase tops out at 63, so 64 can only be a raw PSK, which
           the driver takes as hex digits and nothing else. */
        if (pass_len == 64) {
            for (size_t i = 0; i < pass_len; i++)
                if (hex_val(out->wifi_pass[i]) < 0)
                    return setup_fail(out, MQTT_FORM_ERR_BAD_CHAR, MQTT_FORM_FIELD_WIFI_PASS);
        }
        out->has_wifi = true;
    } else {
        memset(out->wifi_pass, 0, sizeof(out->wifi_pass));
    }

    if (out->mqtt.uri[0] != '\0') {
        st = finalize(&out->mqtt);
        if (st.err != MQTT_FORM_ERR_NONE)
            return setup_fail(out, st.err, st.field);
        out->has_mqtt = true;
    } else {
        memset(&out->mqtt, 0, sizeof(out->mqtt));
    }
    return (mqtt_form_status_t){MQTT_FORM_ERR_NONE, MQTT_FORM_FIELD_NONE};
}

/* ---- JSON body -------------------------------------------------------- */

/* True if body contains a `\u0000` escape (any case on the 'u') anywhere
   -- including, harmlessly, inside what will turn out to be a string
   value. cJSON does not expose a parsed string's original length, only
   its NUL-terminated valuestring, so comparing decoded-vs-source length
   is not available here; scanning the raw source for the escape is the
   alternative the header documents, and it can false-positive on an
   escaped backslash immediately followed by literal "u0000" -- rejecting
   that is harmless. A decoded \u0000 would silently truncate whatever
   later reads the field as a C string, the same hazard a raw embedded
   NUL byte is, so both are checked before cJSON ever runs. */
static bool body_has_nul_escape(const char *body, size_t body_len) {
    if (body_len < 6)
        return false;
    for (size_t i = 0; i + 6 <= body_len; i++)
        if (body[i] == '\\' && (body[i + 1] == 'u' || body[i + 1] == 'U') && body[i + 2] == '0' && body[i + 3] == '0' &&
            body[i + 4] == '0' && body[i + 5] == '0')
            return true;
    return false;
}

/* Bounded pre-scan, run before cJSON ever sees the bytes: this format's
   only valid shape is one flat object of string values, so anything
   nesting a `[`/`{` past depth 1 is structurally invalid and is rejected
   here rather than handed to cJSON's recursive-descent parser. Honours
   string state and backslash escapes, so brackets inside a string value
   (which cJSON must still accept) are never mistaken for nesting -- the
   scan tracks "inside a string" the same way a JSON tokenizer does: an
   unescaped `"` toggles it, and a `\` inside a string escapes the next
   byte so an escaped quote cannot end the string early. */
static bool json_nesting_too_deep(const char *body, size_t body_len) {
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    for (size_t i = 0; i < body_len; i++) {
        char c = body[i];
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '[' || c == '{') {
            if (++depth > 1)
                return true;
        } else if (c == ']' || c == '}') {
            depth--;
        }
    }
    return false;
}

/* True when every byte from p (exclusive of nothing, inclusive of p) up
   to end is JSON whitespace. Used to require that nothing but trailing
   whitespace follows the one object cJSON_ParseWithLengthOpts parsed, so
   a second JSON value or arbitrary trailing bytes cannot ride along
   unexamined. */
static bool only_json_whitespace(const char *p, const char *end) {
    for (; p < end; p++)
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            return false;
    return true;
}

/* True when name appears more than once as a top-level key of root,
   compared case-sensitively (the same sense cJSON_GetObjectItemCaseSensitive
   already looks up a key in) -- regardless of what type any of the
   duplicates' values are, so `{"uri":"a","uri":5}` is caught here before
   the second occurrence's type is ever inspected. */
static bool json_has_duplicate(const cJSON *root, const char *name) {
    int count = 0;
    for (const cJSON *item = root->child; item != NULL; item = item->next)
        if (item->string != NULL && strcmp(item->string, name) == 0)
            count++;
    return count > 1;
}

static mqtt_form_status_t copy_json_str(const char *s, char *dst, size_t dst_cap, mqtt_form_field_t field) {
    size_t len = strlen(s);
    if (len >= dst_cap)
        return (mqtt_form_status_t){MQTT_FORM_ERR_TOO_LONG, field};
    memcpy(dst, s, len + 1);
    return (mqtt_form_status_t){MQTT_FORM_ERR_NONE, MQTT_FORM_FIELD_NONE};
}

mqtt_form_status_t mqtt_form_parse_json(const char *body, size_t body_len, mqtt_form_result_t *out) {
    if (body_len > MQTT_FORM_BODY_MAX)
        return fail(out, MQTT_FORM_ERR_BODY_TOO_LONG, MQTT_FORM_FIELD_NONE);
    if (memchr(body, '\0', body_len) != NULL)
        return fail(out, MQTT_FORM_ERR_BAD_JSON, MQTT_FORM_FIELD_NONE);
    if (body_has_nul_escape(body, body_len))
        return fail(out, MQTT_FORM_ERR_BAD_JSON, MQTT_FORM_FIELD_NONE);
    if (json_nesting_too_deep(body, body_len))
        return fail(out, MQTT_FORM_ERR_BAD_JSON, MQTT_FORM_FIELD_NONE);

    memset(out, 0, sizeof(*out));

    /* require_null_terminated is 0: body_len is authoritative and body
       need not carry a trailing NUL, matching the urlencoded parser's
       contract and what a protocomm/httpd body buffer actually looks
       like. parse_end lets the caller see what cJSON did NOT consume, so
       trailing garbage after the one object can be rejected below rather
       than silently ignored. */
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(body, body_len, &parse_end, 0);
    if (root == NULL || !cJSON_IsObject(root) || !only_json_whitespace(parse_end, body + body_len)) {
        cJSON_Delete(root);
        return fail(out, MQTT_FORM_ERR_BAD_JSON, MQTT_FORM_FIELD_NONE);
    }

    if (json_has_duplicate(root, "uri")) {
        cJSON_Delete(root);
        return fail(out, MQTT_FORM_ERR_DUPLICATE_FIELD, MQTT_FORM_FIELD_URI);
    }
    if (json_has_duplicate(root, "user")) {
        cJSON_Delete(root);
        return fail(out, MQTT_FORM_ERR_DUPLICATE_FIELD, MQTT_FORM_FIELD_USER);
    }
    if (json_has_duplicate(root, "pass")) {
        cJSON_Delete(root);
        return fail(out, MQTT_FORM_ERR_DUPLICATE_FIELD, MQTT_FORM_FIELD_PASS);
    }

    const cJSON *uri_item = cJSON_GetObjectItemCaseSensitive(root, "uri");
    const cJSON *user_item = cJSON_GetObjectItemCaseSensitive(root, "user");
    const cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "pass");

    mqtt_form_status_t st;
    if (uri_item == NULL) {
        st = (mqtt_form_status_t){MQTT_FORM_ERR_MISSING, MQTT_FORM_FIELD_URI};
    } else if (!cJSON_IsString(uri_item)) {
        st = (mqtt_form_status_t){MQTT_FORM_ERR_BAD_JSON, MQTT_FORM_FIELD_URI};
    } else if (user_item != NULL && !cJSON_IsString(user_item)) {
        st = (mqtt_form_status_t){MQTT_FORM_ERR_BAD_JSON, MQTT_FORM_FIELD_USER};
    } else if (pass_item != NULL && !cJSON_IsString(pass_item)) {
        st = (mqtt_form_status_t){MQTT_FORM_ERR_BAD_JSON, MQTT_FORM_FIELD_PASS};
    } else {
        st = copy_json_str(uri_item->valuestring, out->uri, MQTT_FORM_URI_MAX, MQTT_FORM_FIELD_URI);
        if (st.err == MQTT_FORM_ERR_NONE && user_item != NULL)
            st = copy_json_str(user_item->valuestring, out->user, MQTT_FORM_USER_MAX, MQTT_FORM_FIELD_USER);
        if (st.err == MQTT_FORM_ERR_NONE && pass_item != NULL)
            st = copy_json_str(pass_item->valuestring, out->pass, MQTT_FORM_PASS_MAX, MQTT_FORM_FIELD_PASS);
    }

    cJSON_Delete(root);
    if (st.err != MQTT_FORM_ERR_NONE)
        return fail(out, st.err, st.field);
    return finalize(out);
}

/* ---- error text and HTML escaping ------------------------------------ */

const char *mqtt_form_error_str(mqtt_form_err_t err) {
    switch (err) {
        case MQTT_FORM_ERR_NONE:
            return "";
        case MQTT_FORM_ERR_MISSING:
            return "required field is missing";
        case MQTT_FORM_ERR_TOO_LONG:
            return "value is too long";
        case MQTT_FORM_ERR_BAD_SCHEME:
            return "must start with mqtt:// or mqtts://";
        case MQTT_FORM_ERR_BAD_CHAR:
            return "contains a character that is not allowed";
        case MQTT_FORM_ERR_MALFORMED_ENCODING:
            return "malformed percent-encoding in request";
        case MQTT_FORM_ERR_BODY_TOO_LONG:
            return "request body is too long";
        case MQTT_FORM_ERR_DUPLICATE_FIELD:
            return "field was submitted twice";
        case MQTT_FORM_ERR_BAD_JSON:
            return "malformed JSON request";
        case MQTT_FORM_ERR_TOO_SHORT:
            return "value is too short";
        default:
            break;
    }
    return "";
}

bool mqtt_form_html_escape(const char *in, char *out, size_t out_len) {
    if (out != NULL && out_len > 0)
        out[0] = '\0';
    if (in == NULL || out == NULL || out_len == 0)
        return false;

    size_t o = 0;
    for (const char *p = in; *p != '\0'; p++) {
        const char *rep;
        switch (*p) {
            case '&':
                rep = "&amp;";
                break;
            case '<':
                rep = "&lt;";
                break;
            case '>':
                rep = "&gt;";
                break;
            case '"':
                rep = "&quot;";
                break;
            case '\'':
                rep = "&#39;";
                break;
            default:
                rep = NULL;
                break;
        }
        if (rep != NULL) {
            size_t rep_len = strlen(rep);
            if (o + rep_len >= out_len) {
                out[0] = '\0';
                return false;
            }
            memcpy(out + o, rep, rep_len);
            o += rep_len;
        } else {
            if (o + 1 >= out_len) {
                out[0] = '\0';
                return false;
            }
            out[o++] = *p;
        }
    }
    out[o] = '\0';
    return true;
}
