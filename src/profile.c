/* SPDX-License-Identifier: MIT */
/* Profile persistence: JSON save/load with a small self-contained parser
 * (no external JSON dependency). The on-disk format is:
 *
 * {
 *   "version": 1,
 *   "rules": [
 *     {"iface":"eth0","src_ip":"","src_port":"80","ip":"1.2.3.4","port":"443",
 *      "bandwidth":"100kbit","latency_ms":100,"jitter_ms":20,"drop_pct":5,
 *      "active":false},
 *     ...
 *   ]
 * }
 *
 * "ip"/"port" are the destination match, named that way since before source
 * matching existed; profiles written by older builds load unchanged, with
 * empty source fields.
 */
#include "profile.h"

#include "tc.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void profile_clear(LagProfile *p) {
    for (int i = 0; i < LAG_MAX_RULES; i++)
        lag_rule_reset(&p->rules[i]);
    p->count = 0;
}

char *profile_default_path(void) {
    const char *home = g_get_home_dir();
    char *dir = g_build_filename(home, ".config", "testlag", NULL);
    char *path = g_build_filename(dir, "profile.json", NULL);
    g_free(dir);
    return path;
}

/* ---------------- JSON emitter ---------------- */

static void json_escape(GString *s, const char *str) {
    g_string_append_c(s, '"');
    for (const char *c = str; *c; c++) {
        switch (*c) {
        case '"':  g_string_append(s, "\\\""); break;
        case '\\': g_string_append(s, "\\\\"); break;
        case '\n': g_string_append(s, "\\n");  break;
        case '\t': g_string_append(s, "\\t");  break;
        case '\r': g_string_append(s, "\\r");  break;
        default:
            if ((unsigned char)*c < 0x20)
                g_string_append_printf(s, "\\u%04x", (unsigned char)*c);
            else
                g_string_append_c(s, *c);
        }
    }
    g_string_append_c(s, '"');
}

static void append_num(GString *s, double v) {
    char buf[64];
    if (!isfinite(v))          /* never write nan/inf: it would not parse back */
        v = 0;
    if (v > -1e15 && v < 1e15 && v == (double)(long long)v)
        g_snprintf(buf, sizeof buf, "%lld", (long long)v);
    else {
        g_snprintf(buf, sizeof buf, "%.3f", v);
        char *dot = strchr(buf, '.');
        if (dot) {
            char *end = buf + strlen(buf) - 1;
            while (end > dot && *end == '0') *end-- = '\0';
            if (*end == '.') *end = '\0';
        }
    }
    g_string_append(s, buf);
}

int profile_save(const LagProfile *p, const char *path, char *err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    GString *s = g_string_new("{\n  \"version\": 2,\n  \"rules\": [");
    for (int i = 0; i < p->count; i++) {
        const LagRule *r = &p->rules[i];
        g_string_append(s, i ? ",\n    {" : "\n    {");
        g_string_append(s, "\"iface\":");      json_escape(s, r->iface);
        g_string_append(s, ",\"direction\":"); json_escape(s, lag_direction_name(r->direction));
        g_string_append(s, ",\"protocol\":"); json_escape(s, lag_protocol_name(r->protocol));
        g_string_append(s, ",\"src_ip\":");    json_escape(s, r->src_ip);
        g_string_append(s, ",\"src_port\":");  json_escape(s, r->src_port);
        g_string_append(s, ",\"ip\":");        json_escape(s, r->dst_ip);
        g_string_append(s, ",\"port\":");      json_escape(s, r->dst_port);
        g_string_append(s, ",\"bandwidth\":"); json_escape(s, r->bandwidth);
        g_string_append(s, ",\"latency_ms\":"); append_num(s, r->latency_ms);
        g_string_append(s, ",\"jitter_ms\":");  append_num(s, r->jitter_ms);
        g_string_append(s, ",\"drop_pct\":"); append_num(s, r->drop_pct);
        g_string_append(s, r->active ? ",\"active\":true" : ",\"active\":false");
        g_string_append_c(s, '}');
    }
    g_string_append(s, p->count ? "\n  ]\n}\n" : "  ]\n}\n");

    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    GError *gerr = NULL;
    if (!g_file_set_contents(path, s->str, -1, &gerr)) {
        g_snprintf(err, errlen, "%s", gerr ? gerr->message : "write failed");
        g_clear_error(&gerr);
        g_string_free(s, TRUE);
        return -1;
    }
    g_string_free(s, TRUE);
    return 0;
}

/* ---------------- minimal JSON parser (trusted, our own format) ---------------- */

/* Nesting limit for the skip-any-value path: without it a profile of the
 * form {"x":[[[[... recurses once per bracket and exhausts the stack. */
#define JP_MAX_DEPTH 64

typedef struct {
    const char *p;
    const char *end;
    int err;
} JP;

static void jp_ws(JP *j) {
    while (j->p < j->end && g_ascii_isspace((unsigned char)*j->p))
        j->p++;
}

/* Parse a JSON string; *j->p must point at the opening quote. */
static char *jp_string(JP *j) {
    if (j->p >= j->end || *j->p != '"') { j->err = 1; return NULL; }
    j->p++;
    GString *s = g_string_new("");
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            char e = *j->p++;
            switch (e) {
            case '"':  g_string_append_c(s, '"');  break;
            case '\\': g_string_append_c(s, '\\'); break;
            case '/':  g_string_append_c(s, '/');  break;
            case 'n':  g_string_append_c(s, '\n'); break;
            case 't':  g_string_append_c(s, '\t'); break;
            case 'r':  g_string_append_c(s, '\r'); break;
            case 'b':  g_string_append_c(s, '\b'); break;
            case 'f':  g_string_append_c(s, '\f'); break;
            case 'u':
                if (j->p + 4 <= j->end) {
                    char hex[5] = {0};
                    memcpy(hex, j->p, 4);
                    j->p += 4;
                    long cp = strtol(hex, NULL, 16);
                    if (cp < 0x80) {
                        g_string_append_c(s, (char)cp);
                    } else if (cp < 0x800) {
                        g_string_append_c(s, (char)(0xC0 | (cp >> 6)));
                        g_string_append_c(s, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        g_string_append_c(s, (char)(0xE0 | (cp >> 12)));
                        g_string_append_c(s, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        g_string_append_c(s, (char)(0x80 | (cp & 0x3F)));
                    }
                }
                break;
            default: g_string_append_c(s, e); break;
            }
        } else {
            g_string_append_c(s, c);
        }
    }
    if (j->p >= j->end || *j->p != '"') {
        j->err = 1;
        g_string_free(s, TRUE);
        return NULL;
    }
    j->p++;
    return g_string_free(s, FALSE);
}

/* Skip any JSON value (string/number/bool/null/object/array). */
static void jp_skip_value(JP *j, int depth) {
    if (depth > JP_MAX_DEPTH) { j->err = 1; return; }
    jp_ws(j);
    if (j->p >= j->end) { j->err = 1; return; }
    char c = *j->p;
    if (c == '"') { char *s = jp_string(j); g_free(s); return; }
    if (c == '{') {
        j->p++;
        jp_ws(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return; }
        while (j->p < j->end) {
            jp_ws(j);
            char *k = jp_string(j);
            g_free(k);
            if (j->err) return;
            jp_ws(j);
            if (j->p >= j->end || *j->p != ':') { j->err = 1; return; }
            j->p++;
            jp_skip_value(j, depth + 1);
            if (j->err) return;
            jp_ws(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == '}') { j->p++; return; }
            j->err = 1; return;
        }
        return;
    }
    if (c == '[') {
        j->p++;
        jp_ws(j);
        if (j->p < j->end && *j->p == ']') { j->p++; return; }
        while (j->p < j->end) {
            jp_skip_value(j, depth + 1);
            if (j->err) return;
            jp_ws(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == ']') { j->p++; return; }
            j->err = 1; return;
        }
        return;
    }
    /* number / true / false / null */
    while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']'
           && !g_ascii_isspace((unsigned char)*j->p))
        j->p++;
}

static double jp_number(JP *j) {
    char *endp;
    double v = strtod(j->p, &endp);
    if (endp == j->p) { j->err = 1; return 0; }
    j->p = endp;
    return v;
}

/* Parse one rule object; *j->p must point at '{'. */
static void jp_rule_object(JP *j, LagRule *r) {
    lag_rule_reset(r);
    jp_ws(j);
    if (j->p >= j->end || *j->p != '{') { j->err = 1; return; }
    j->p++;
    jp_ws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return; }
    while (j->p < j->end) {
        jp_ws(j);
        char *key = jp_string(j);
        if (!key) return;
        jp_ws(j);
        if (j->p >= j->end || *j->p != ':') { g_free(key); j->err = 1; return; }
        j->p++;
        jp_ws(j);
        if (g_strcmp0(key, "iface") == 0) {
            char *v = jp_string(j);
            if (v) { g_strlcpy(r->iface, v, LAG_IFACE_LEN); g_free(v); }
        } else if (g_strcmp0(key, "direction") == 0) {
            char *v = jp_string(j);
            if (!tc_parse_direction(v, &r->direction)) r->direction = -1;
            g_free(v);
        } else if (g_strcmp0(key, "protocol") == 0) {
            char *v = jp_string(j);
            if (!tc_parse_protocol(v, &r->protocol)) r->protocol = -1;
            g_free(v);
        } else if (g_strcmp0(key, "src_ip") == 0) {
            char *v = jp_string(j);
            if (v) { g_strlcpy(r->src_ip, v, LAG_IP_LEN); g_free(v); }
        } else if (g_strcmp0(key, "src_port") == 0) {
            char *v = jp_string(j);
            if (v) { g_strlcpy(r->src_port, v, LAG_PORT_LEN); g_free(v); }
        } else if (g_strcmp0(key, "ip") == 0) {
            char *v = jp_string(j);
            if (v) { g_strlcpy(r->dst_ip, v, LAG_IP_LEN); g_free(v); }
        } else if (g_strcmp0(key, "port") == 0) {
            char *v = jp_string(j);
            if (v) { g_strlcpy(r->dst_port, v, LAG_PORT_LEN); g_free(v); }
        } else if (g_strcmp0(key, "bandwidth") == 0) {
            char *v = jp_string(j);
            if (v) { g_strlcpy(r->bandwidth, v, LAG_BW_LEN); g_free(v); }
        } else if (g_strcmp0(key, "latency_ms") == 0) {
            r->latency_ms = jp_number(j);
        } else if (g_strcmp0(key, "jitter_ms") == 0) {
            r->jitter_ms = jp_number(j);
        } else if (g_strcmp0(key, "drop_pct") == 0) {
            r->drop_pct = jp_number(j);
        } else if (g_strcmp0(key, "active") == 0) {
            if (j->p + 4 <= j->end && g_str_has_prefix(j->p, "true")) {
                r->active = TRUE;
                j->p += 4;
            } else if (j->p + 5 <= j->end && g_str_has_prefix(j->p, "false")) {
                r->active = FALSE;
                j->p += 5;
            } else {
                j->err = 1;
            }
        } else {
            jp_skip_value(j, 0);
        }
        g_free(key);
        if (j->err) return;
        jp_ws(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == '}') { j->p++; return; }
        j->err = 1; return;
    }
    j->err = 1;
}

/* Note a rule that was parsed but not kept, without letting the message grow
 * without bound. */
static void note_skipped(GString *warn, int index, const char *why) {
    if (!warn) return;
    if (warn->len > 300) return;
    g_string_append_printf(warn, "%srule %d skipped (%s)",
                           warn->len ? "; " : "", index, why);
}

/* *j->p must point at '['. Parse the rules array.
 * A rule is kept only if every field passes validation: a profile file is
 * data, not a trusted source of tc command arguments. Rejected rules are
 * reported through warn and the rest of the profile still loads. */
static void jp_rules_array(JP *j, LagProfile *p, GString *warn) {
    j->p++; /* consume '[' */
    jp_ws(j);
    if (j->p < j->end && *j->p == ']') { j->p++; return; }
    if (j->p >= j->end || *j->p != '{') { j->err = 1; return; }
    for (int index = 0; ; index++) {
        LagRule r;
        jp_rule_object(j, &r);
        if (j->err) return;
        char verr[256];
        if (!tc_valid_rule(&r, verr, sizeof verr))
            note_skipped(warn, index, verr);
        else if (p->count >= LAG_MAX_RULES)
            note_skipped(warn, index, "rule limit reached");
        else
            p->rules[p->count++] = r;
        jp_ws(j);
        if (j->p >= j->end) { j->err = 1; return; }
        if (*j->p == ']') { j->p++; return; }
        if (*j->p != ',') { j->err = 1; return; }
        j->p++;
        jp_ws(j);
        if (j->p >= j->end || *j->p != '{') { j->err = 1; return; }
    }
}

int profile_load(LagProfile *p, const char *path, char *err, size_t errlen) {
    profile_clear(p);
    if (err && errlen) err[0] = '\0';

    char *content = NULL;
    gsize len = 0;
    GError *gerr = NULL;
    if (!g_file_get_contents(path, &content, &len, &gerr)) {
        g_snprintf(err, errlen, "%s", gerr ? gerr->message : "read failed");
        g_clear_error(&gerr);
        return -1;
    }

    GString *warn = g_string_new("");
    JP j = { content, content + len, 0 };
    jp_ws(&j);
    if (j.p >= j.end || *j.p != '{') {
        g_free(content);
        g_string_free(warn, TRUE);
        g_snprintf(err, errlen, "not a JSON object");
        return -1;
    }
    j.p++;
    jp_ws(&j);
    if (j.p >= j.end) {
        j.err = 1;
    } else if (*j.p == '}') {
        j.p++; /* empty object */
    } else {
        for (;;) {
            jp_ws(&j);
            char *key = jp_string(&j);
            if (!key) break;
            jp_ws(&j);
            if (j.p >= j.end || *j.p != ':') { g_free(key); j.err = 1; break; }
            j.p++;
            jp_ws(&j);
            if (g_strcmp0(key, "rules") == 0) {
                if (j.p >= j.end || *j.p != '[') { g_free(key); j.err = 1; break; }
                jp_rules_array(&j, p, warn);
            } else {
                jp_skip_value(&j, 0);
            }
            g_free(key);
            if (j.err) break;
            jp_ws(&j);
            if (j.p >= j.end) { j.err = 1; break; }
            if (*j.p == '}') { j.p++; break; }
            if (*j.p != ',') { j.err = 1; break; }
            j.p++;
        }
    }

    g_free(content);
    if (j.err) {
        g_string_free(warn, TRUE);
        profile_clear(p);
        g_snprintf(err, errlen, "JSON parse error in %s", path);
        return -1;
    }
    if (warn->len && err && errlen)
        g_snprintf(err, errlen, "%s", warn->str);
    g_string_free(warn, TRUE);
    return 0;
}
