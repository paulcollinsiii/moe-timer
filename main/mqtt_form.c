/* Setup-mode MQTT entry point — see include/mqtt_form.h for the module's
   job and the empty-URI contract. Two parse entry points funnel into one
   `finalize()` so the form page and the JSON endpoint can never accept a
   value the other would refuse. */
#include "mqtt_form.h"

#include <ctype.h>
#include <string.h>

#include "cJSON.h"
#include "config_validate.h" /* config_is_mqtt_uri */

/* ---- shared field-value checks ------------------------------------- */

/* True if s contains a control character (< 0x20). Deliberately not
   config_is_clean_str: that rule also forbids '"' and '\\', which this
   field's spec does not ask for (a username or password containing
   either is unusual but not a device-level hazard — these values are
   never interpolated into hand-built JSON the way a config field is). */
static bool has_control_char(const char *s) {
    for (; *s != '\0'; s++)
        if ((unsigned char)*s < 0x20)
            return true;
    return false;
}

/* Case-insensitive prefix compare, matching config_is_mqtt_uri's own
   scheme rule exactly (it uses tolower() too) — a case mismatch here
   would mis-report a BAD_CHAR uri ("MQTT://ho st") as BAD_SCHEME instead,
   because the scheme check would wrongly fail before the character
   check ever runs. */
static bool scheme_prefix_eq_ci(const char *s, const char *scheme, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (tolower((unsigned char)s[i]) != scheme[i])
            return false;
    return true;
}

/* Diagnostic only — config_is_mqtt_uri() already made the one accept/
   reject call in finalize() below. This just separates its two failure
   reasons (wrong/absent scheme vs. bad character in an otherwise valid
   URI) so the form page can say which. A URI with the right scheme but
   no host is reported as BAD_SCHEME, matching config_is_mqtt_uri's own
   "scheme but no host" refusal. */
static bool has_mqtt_scheme_and_host(const char *s) {
    static const char scheme_mqtts[] = "mqtts://";
    static const char scheme_mqtt[] = "mqtt://";
    size_t len;
    if (scheme_prefix_eq_ci(s, scheme_mqtts, sizeof(scheme_mqtts) - 1))
        len = sizeof(scheme_mqtts) - 1;
    else if (scheme_prefix_eq_ci(s, scheme_mqtt, sizeof(scheme_mqtt) - 1))
        len = sizeof(scheme_mqtt) - 1;
    else
        return false;
    return s[len] != '\0';
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

    if (!config_is_mqtt_uri(out->uri)) {
        if (!has_mqtt_scheme_and_host(out->uri))
            return (mqtt_form_status_t){MQTT_FORM_ERR_BAD_SCHEME, MQTT_FORM_FIELD_URI};
        return (mqtt_form_status_t){MQTT_FORM_ERR_BAD_CHAR, MQTT_FORM_FIELD_URI};
    }

    if (has_control_char(out->user))
        return (mqtt_form_status_t){MQTT_FORM_ERR_BAD_CHAR, MQTT_FORM_FIELD_USER};

    if (out->pass[0] == '\0') {
        out->keep_pass = true;
    } else {
        if (has_control_char(out->pass))
            return (mqtt_form_status_t){MQTT_FORM_ERR_BAD_CHAR, MQTT_FORM_FIELD_PASS};
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

mqtt_form_status_t mqtt_form_parse_urlencoded(const char *body, size_t body_len, mqtt_form_result_t *out) {
    if (body_len > MQTT_FORM_BODY_MAX)
        return (mqtt_form_status_t){MQTT_FORM_ERR_BODY_TOO_LONG, MQTT_FORM_FIELD_NONE};

    memset(out, 0, sizeof(*out));
    bool uri_seen = false, user_seen = false, pass_seen = false, uri_present = false;

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

        char *dst;
        size_t dst_cap;
        bool *seen;
        mqtt_form_field_t field;
        if (key_len == 3 && memcmp(key, "uri", 3) == 0) {
            dst = out->uri;
            dst_cap = MQTT_FORM_URI_MAX;
            seen = &uri_seen;
            field = MQTT_FORM_FIELD_URI;
        } else if (key_len == 4 && memcmp(key, "user", 4) == 0) {
            dst = out->user;
            dst_cap = MQTT_FORM_USER_MAX;
            seen = &user_seen;
            field = MQTT_FORM_FIELD_USER;
        } else if (key_len == 4 && memcmp(key, "pass", 4) == 0) {
            dst = out->pass;
            dst_cap = MQTT_FORM_PASS_MAX;
            seen = &pass_seen;
            field = MQTT_FORM_FIELD_PASS;
        } else {
            continue; /* unknown field: ignored, per the module contract */
        }

        if (*seen)
            return (mqtt_form_status_t){MQTT_FORM_ERR_DUPLICATE_FIELD, field};
        *seen = true;
        if (field == MQTT_FORM_FIELD_URI)
            uri_present = true;

        mqtt_form_err_t derr = decode_value(val, val_len, dst, dst_cap);
        if (derr != MQTT_FORM_ERR_NONE)
            return (mqtt_form_status_t){derr, field};
    }

    if (!uri_present)
        return (mqtt_form_status_t){MQTT_FORM_ERR_MISSING, MQTT_FORM_FIELD_URI};

    return finalize(out);
}

/* ---- JSON body -------------------------------------------------------- */

static mqtt_form_status_t copy_json_str(const char *s, char *dst, size_t dst_cap, mqtt_form_field_t field) {
    size_t len = strlen(s);
    if (len >= dst_cap)
        return (mqtt_form_status_t){MQTT_FORM_ERR_TOO_LONG, field};
    memcpy(dst, s, len + 1);
    return (mqtt_form_status_t){MQTT_FORM_ERR_NONE, MQTT_FORM_FIELD_NONE};
}

mqtt_form_status_t mqtt_form_parse_json(const char *body, size_t body_len, mqtt_form_result_t *out) {
    if (body_len > MQTT_FORM_BODY_MAX)
        return (mqtt_form_status_t){MQTT_FORM_ERR_BODY_TOO_LONG, MQTT_FORM_FIELD_NONE};

    memset(out, 0, sizeof(*out));

    /* require_null_terminated is 0 behind cJSON_ParseWithLength: body_len
       is authoritative and body need not carry a trailing NUL, matching
       the urlencoded parser's contract and what a protocomm/httpd body
       buffer actually looks like. */
    cJSON *root = cJSON_ParseWithLength(body, body_len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return (mqtt_form_status_t){MQTT_FORM_ERR_BAD_JSON, MQTT_FORM_FIELD_NONE};
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
        return st;
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
