/* SPDX-License-Identifier: MIT */
/* lag CLI mode: manage profile rules and live tc state without the GUI.
 *
 * Commands:
 *   list          list rules in the profile
 *   ifaces        list available network interfaces
 *   add           add a rule (saved to the profile)
 *   edit N        update rule N (unspecified fields unchanged)
 *   del N         delete rule N (stops it first if active)
 *   start N|all   apply rule(s) via tc
 *   stop  N|all   remove rule(s) from tc
 *   status        show live tc state for the profile's interfaces
 *   help          show command help
 *
 * Semantics mirror the GUI handlers in ui.c: starting/stopping a rule
 * rebuilds the whole qdisc tree for its interface from all active rules,
 * and every mutating command saves the profile afterwards. */
#include "cli.h"

#include "lag.h"
#include "profile.h"
#include "tc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- usage ---------------- */

static void cli_usage(FILE *out) {
    fprintf(out,
        "Usage: testlag [options] <command> [args]\n"
        "\n"
        "Commands:\n"
        "  list                            List rules in the profile\n"
        "  ifaces                          List available network interfaces\n"
        "  add --iface IFACE [rule opts]   Add a rule (saved to the profile)\n"
        "  edit N [rule opts]              Update rule N (unspecified fields unchanged)\n"
        "  del N                           Delete rule N (stops it first if active)\n"
        "  start N | all                   Apply rule(s) via tc\n"
        "  stop  N | all                   Remove rule(s) from tc\n"
        "  status                          Show live tc state for profile interfaces\n"
        "  help                            Show this help\n"
        "\n"
        "Rule options (source/destination refer to the packet endpoints):\n"
        "  --direction DIR   outgoing (default) or incoming\n"
        "  --protocol PROTO  any (default), tcp, udp, sctp, icmp, icmpv6\n"
        "  --iface IFACE     Network interface (required for add; e.g. lo, eth0)\n"
        "  --src-ip IP       Source IP or CIDR subnet (default: any)\n"
        "  --src-port PORT   Source port, e.g. 80 for your own web server\n"
        "  --dst-ip IP       Destination IP or CIDR (default: any); --ip is an alias\n"
        "  --dst-port PORT   Destination port (default: any); --port is an alias\n"
        "  --latency MS      Latency in milliseconds, 0-60000 (default: 0)\n"
        "  --jitter MS       Jitter in milliseconds, 0-60000 (default: 0)\n"
        "  --drop PCT        Packet loss percent, 0-100 (default: 0)\n"
        "  --bandwidth RATE  Netem rate, e.g. 100kbit, 1mbit (default: unlimited)\n"
        "  --active          (add only) start the rule immediately\n"
        "\n"
        "Examples:\n"
        "  testlag list\n"
        "  testlag add --iface lo --dst-ip 192.168.1.50 --dst-port 443 --latency 150 \\\n"
        "      --jitter 30 --drop 5 --bandwidth 100kbit\n"
        "  # lag everyone hitting this machine's web server (delays its replies):\n"
        "  testlag add --iface eth0 --src-port 80 --latency 200\n"
        "  # lag just one device talking to that web server:\n"
        "  testlag add --iface eth0 --src-port 80 --dst-ip 192.168.1.77 --latency 200\n"
        "  testlag start 0\n"
        "  testlag stop all\n"
        "  testlag del 2\n");
}

gboolean cli_is_command(const char *s) {
    static const char *cmds[] = { "list", "add", "edit", "del", "delete",
                                  "start", "stop", "status", "ifaces",
                                  "help", NULL };
    for (int i = 0; cmds[i]; i++)
        if (g_strcmp0(s, cmds[i]) == 0)
            return TRUE;
    return FALSE;
}

/* ---------------- rule option parsing ---------------- */

typedef struct {
    gboolean has_iface, has_bw, has_lat, has_jit, has_drop;
    gboolean has_src_ip, has_src_port, has_dst_ip, has_dst_port;
    gboolean active;
    gboolean has_direction, has_protocol;
    LagDirection direction;
    LagProtocol protocol;
    char     iface[LAG_IFACE_LEN];
    char     src_ip[LAG_IP_LEN], src_port[LAG_PORT_LEN];
    char     dst_ip[LAG_IP_LEN], dst_port[LAG_PORT_LEN];
    char     bw[LAG_BW_LEN];
    double   lat, jit, drop;
} RuleOpts;

static gboolean parse_double_opt(const char *s, double *out) {
    if (!s) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (end == s || *end != '\0' || v < 0)
        return FALSE;
    *out = v;
    return TRUE;
}

/* Parse rule options (argv[0] = first option). Returns 0 on success,
 * -1 with err filled on failure. */
static int parse_rule_opts(char **argv, RuleOpts *o, char *err, size_t errlen) {
    for (int i = 0; argv[i] != NULL; i++) {
        const char *a = argv[i];
        if (g_strcmp0(a, "--direction") == 0) {
            i++;
            if (!tc_parse_direction(argv[i], &o->direction)) {
                g_snprintf(err, errlen, "--direction must be outgoing or incoming"); return -1;
            }
            o->has_direction = TRUE;
        } else if (g_strcmp0(a, "--protocol") == 0) {
            i++;
            if (!tc_parse_protocol(argv[i], &o->protocol)) {
                g_snprintf(err, errlen, "--protocol must be any, tcp, udp, sctp, icmp or icmpv6"); return -1;
            }
            o->has_protocol = TRUE;
        } else if (g_strcmp0(a, "--iface") == 0) {
            i++;
            if (!argv[i]) { g_snprintf(err, errlen, "--iface requires a value"); return -1; }
            if (!tc_valid_iface(argv[i])) {
                g_snprintf(err, errlen, "invalid interface name: %s", argv[i]);
                return -1;
            }
            g_strlcpy(o->iface, argv[i], sizeof o->iface);
            o->has_iface = TRUE;
        } else if (g_strcmp0(a, "--src-ip") == 0) {
            i++;
            if (!argv[i]) { g_snprintf(err, errlen, "--src-ip requires a value"); return -1; }
            if (!tc_valid_ip(argv[i])) {
                g_snprintf(err, errlen, "invalid source IP address: %s", argv[i]);
                return -1;
            }
            g_strlcpy(o->src_ip, argv[i], sizeof o->src_ip);
            o->has_src_ip = TRUE;
        } else if (g_strcmp0(a, "--src-port") == 0) {
            i++;
            if (!argv[i]) { g_snprintf(err, errlen, "--src-port requires a value"); return -1; }
            if (!tc_valid_port(argv[i])) {
                g_snprintf(err, errlen, "invalid source port: %s", argv[i]);
                return -1;
            }
            g_strlcpy(o->src_port, argv[i], sizeof o->src_port);
            o->has_src_port = TRUE;
        } else if (g_strcmp0(a, "--dst-ip") == 0 || g_strcmp0(a, "--ip") == 0) {
            i++;
            if (!argv[i]) { g_snprintf(err, errlen, "%s requires a value", a); return -1; }
            if (!tc_valid_ip(argv[i])) {
                g_snprintf(err, errlen, "invalid destination IP address: %s", argv[i]);
                return -1;
            }
            g_strlcpy(o->dst_ip, argv[i], sizeof o->dst_ip);
            o->has_dst_ip = TRUE;
        } else if (g_strcmp0(a, "--dst-port") == 0 || g_strcmp0(a, "--port") == 0) {
            i++;
            if (!argv[i]) { g_snprintf(err, errlen, "%s requires a value", a); return -1; }
            if (!tc_valid_port(argv[i])) {
                g_snprintf(err, errlen, "invalid destination port: %s", argv[i]);
                return -1;
            }
            g_strlcpy(o->dst_port, argv[i], sizeof o->dst_port);
            o->has_dst_port = TRUE;
        } else if (g_strcmp0(a, "--latency") == 0) {
            i++;
            if (!parse_double_opt(argv[i], &o->lat) || !tc_valid_ms(o->lat)) {
                g_snprintf(err, errlen, "invalid --latency (need 0-%d): %s",
                           LAG_MAX_MS, argv[i] ? argv[i] : "(missing)");
                return -1;
            }
            o->has_lat = TRUE;
        } else if (g_strcmp0(a, "--jitter") == 0) {
            i++;
            if (!parse_double_opt(argv[i], &o->jit) || !tc_valid_ms(o->jit)) {
                g_snprintf(err, errlen, "invalid --jitter (need 0-%d): %s",
                           LAG_MAX_MS, argv[i] ? argv[i] : "(missing)");
                return -1;
            }
            o->has_jit = TRUE;
        } else if (g_strcmp0(a, "--drop") == 0) {
            i++;
            if (!parse_double_opt(argv[i], &o->drop) || !tc_valid_pct(o->drop)) {
                g_snprintf(err, errlen, "invalid --drop (need 0-100): %s", argv[i] ? argv[i] : "(missing)");
                return -1;
            }
            o->has_drop = TRUE;
        } else if (g_strcmp0(a, "--bandwidth") == 0) {
            i++;
            if (!argv[i]) { g_snprintf(err, errlen, "--bandwidth requires a value"); return -1; }
            if (!tc_valid_rate(argv[i])) {
                g_snprintf(err, errlen, "invalid --bandwidth: %s", argv[i]);
                return -1;
            }
            g_strlcpy(o->bw, argv[i], sizeof o->bw);
            o->has_bw = TRUE;
        } else if (g_strcmp0(a, "--active") == 0) {
            o->active = TRUE;
        } else {
            g_snprintf(err, errlen, "unknown option: %s", a);
            return -1;
        }
    }
    return 0;
}

/* Parse a rule index; returns the index or -1 with err filled. */
static int parse_index(const char *s, LagProfile *p, char *err, size_t errlen) {
    if (!s) { g_snprintf(err, errlen, "missing rule index"); return -1; }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v >= p->count) {
        g_snprintf(err, errlen, "invalid rule index: %s (profile has %d rules)",
                   s, p->count);
        return -1;
    }
    return (int)v;
}

/* Save the profile; returns 0 on success, -1 with a message printed on failure. */
static int save_profile(LagProfile *p, const char *path) {
    char err[512];
    if (profile_save(p, path, err, sizeof err) != 0) {
        fprintf(stderr, "error: could not save profile %s: %s\n", path, err);
        return -1;
    }
    return 0;
}

/* ---------------- commands ---------------- */

static int cmd_list(LagProfile *p) {
    if (p->count == 0) {
        printf("(no rules in profile)\n");
        return 0;
    }
    printf(" #  %-8s %-6s %-9s %-15s %-6s %-15s %-6s %-8s %-8s %-8s %-11s %s\n",
           "DIR", "PROTO", "IFACE", "SRC IP", "SPORT", "DST IP", "DPORT",
           "LAT(ms)", "JIT(ms)", "DROP(%)", "BW", "STATE");
    for (int i = 0; i < p->count; i++) {
        const LagRule *r = &p->rules[i];
        printf("%2d  %-8s %-6s %-9s %-15s %-6s %-15s %-6s %-8g %-8g %-8g %-11s %s\n",
               i, lag_direction_name(r->direction), lag_protocol_name(r->protocol),
               r->iface,
               r->src_ip[0] ? r->src_ip : "(any)",
               r->src_port[0] ? r->src_port : "(any)",
               r->dst_ip[0] ? r->dst_ip : "(any)",
               r->dst_port[0] ? r->dst_port : "(any)",
               r->latency_ms, r->jitter_ms, r->drop_pct,
               r->bandwidth[0] ? r->bandwidth : "(unlimited)",
               r->active ? "active" : "stopped");
    }
    return 0;
}

static int cmd_ifaces(void) {
    GPtrArray *arr = tc_list_interfaces();
    for (guint i = 0; i < arr->len; i++)
        printf("%s\n", (const char *)g_ptr_array_index(arr, i));
    g_ptr_array_free(arr, TRUE);
    return 0;
}

static int cmd_add(LagProfile *p, const char *path, char **argv) {
    if (p->count >= LAG_MAX_RULES) {
        fprintf(stderr, "error: profile is full (%d rules)\n", LAG_MAX_RULES);
        return 1;
    }
    RuleOpts o;
    memset(&o, 0, sizeof o);
    char err[512];
    if (parse_rule_opts(argv, &o, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    if (!o.has_iface) {
        fprintf(stderr, "error: --iface is required\n");
        return 1;
    }
    LagRule r;
    lag_rule_reset(&r);
    g_strlcpy(r.iface, o.iface, sizeof r.iface);
    if (o.has_src_ip)   g_strlcpy(r.src_ip, o.src_ip, sizeof r.src_ip);
    if (o.has_src_port) g_strlcpy(r.src_port, o.src_port, sizeof r.src_port);
    if (o.has_dst_ip)   g_strlcpy(r.dst_ip, o.dst_ip, sizeof r.dst_ip);
    if (o.has_dst_port) g_strlcpy(r.dst_port, o.dst_port, sizeof r.dst_port);
    if (o.has_bw)   g_strlcpy(r.bandwidth, o.bw, sizeof r.bandwidth);
    if (o.has_lat)  r.latency_ms = o.lat;
    if (o.has_jit)  r.jitter_ms  = o.jit;
    if (o.has_drop) r.drop_pct   = o.drop;
    if (o.has_direction) r.direction = o.direction;
    if (o.has_protocol) r.protocol = o.protocol;
    if (!tc_valid_rule(&r, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err); return 1;
    }

    int idx = p->count;
    p->rules[idx] = r;
    p->count++;
    if (save_profile(p, path) != 0)
        return 1;
    printf("added rule %d on %s\n", idx, r.iface);

    if (o.active) {
        LagRule *rr = &p->rules[idx];
        rr->active = TRUE;
        char terr[2048];
        if (tc_apply_interface(p, rr->iface, terr, sizeof terr) != 0) {
            rr->active = FALSE;
            save_profile(p, path);
            fprintf(stderr, "error: start failed: %s\n", terr);
            return 1;
        }
        if (save_profile(p, path) != 0)
            return 1;
        printf("started rule %d on %s\n", idx, rr->iface);
    }
    return 0;
}

static int cmd_edit(LagProfile *p, const char *path, int idx, char **argv) {
    RuleOpts o;
    memset(&o, 0, sizeof o);
    char err[512];
    if (parse_rule_opts(argv, &o, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    if (!o.has_iface && !o.has_src_ip && !o.has_src_port && !o.has_dst_ip &&
        !o.has_dst_port && !o.has_bw && !o.has_lat && !o.has_jit && !o.has_drop &&
        !o.has_direction && !o.has_protocol) {
        fprintf(stderr, "error: give at least one rule option to change\n");
        return 1;
    }
    LagRule *r = &p->rules[idx];
    LagRule saved = *r;
    char old_iface[LAG_IFACE_LEN];
    g_strlcpy(old_iface, r->iface, sizeof old_iface);
    if (o.has_iface) g_strlcpy(r->iface, o.iface, sizeof r->iface);
    if (o.has_src_ip)   g_strlcpy(r->src_ip, o.src_ip, sizeof r->src_ip);
    if (o.has_src_port) g_strlcpy(r->src_port, o.src_port, sizeof r->src_port);
    if (o.has_dst_ip)   g_strlcpy(r->dst_ip, o.dst_ip, sizeof r->dst_ip);
    if (o.has_dst_port) g_strlcpy(r->dst_port, o.dst_port, sizeof r->dst_port);
    if (o.has_bw)    g_strlcpy(r->bandwidth, o.bw, sizeof r->bandwidth);
    if (o.has_lat)   r->latency_ms = o.lat;
    if (o.has_jit)   r->jitter_ms  = o.jit;
    if (o.has_drop)  r->drop_pct   = o.drop;
    if (o.has_direction) r->direction = o.direction;
    if (o.has_protocol) r->protocol = o.protocol;
    if (!tc_valid_rule(r, err, sizeof err)) {
        *r = saved;
        fprintf(stderr, "error: %s\n", err); return 1;
    }

    if (save_profile(p, path) != 0)
        return 1;

    if (r->active) {
        char terr[2048];
        if (g_strcmp0(old_iface, r->iface) != 0)
            tc_apply_interface(p, old_iface, terr, sizeof terr);
        if (tc_apply_interface(p, r->iface, terr, sizeof terr) != 0) {
            r->active = FALSE;
            save_profile(p, path);
            fprintf(stderr, "error: re-apply failed (rule deactivated): %s\n", terr);
            return 1;
        }
        printf("updated active rule %d on %s\n", idx, r->iface);
    } else {
        printf("updated rule %d on %s\n", idx, r->iface);
    }
    return 0;
}

static int cmd_del(LagProfile *p, const char *path, int idx) {
    LagRule *r = &p->rules[idx];
    /* keep the name: after the shift below, rules[idx] is a different rule */
    char iface[LAG_IFACE_LEN];
    g_strlcpy(iface, r->iface, sizeof iface);
    if (r->active) {
        r->active = FALSE;
        char err[2048];
        if (tc_apply_interface(p, iface, err, sizeof err) != 0) {
            r->active = TRUE;
            fprintf(stderr, "error: could not stop %s; rule retained: %s\n", iface, err);
            return 1;
        }
    }
    for (int i = idx; i < p->count - 1; i++)
        p->rules[i] = p->rules[i + 1];
    p->count--;
    lag_rule_reset(&p->rules[p->count]);
    if (save_profile(p, path) != 0)
        return 1;
    printf("deleted rule %d on %s\n", idx, iface);
    return 0;
}

/* Start or stop every rule in the profile.
 *
 * tc_apply_interface() rebuilds an interface's whole qdisc tree from all of
 * its active rules, so the flags are flipped first and each interface is
 * converged exactly once. Doing it a rule at a time rebuilt the tree once per
 * rule -- quadratic in the rule count -- and unprivileged every tc command in
 * every rebuild is a separate sudo invocation, i.e. a separate PAM/logind
 * session. Forty rules cost ~2700 processes that way, enough to run the
 * machine out of file descriptors.
 * Returns 0 on success, 1 if any interface failed. */
static int start_stop_all(LagProfile *p, gboolean start) {
    gboolean saved[LAG_MAX_RULES];
    int changed = 0;
    for (int i = 0; i < p->count; i++) {
        saved[i] = p->rules[i].active;
        if (p->rules[i].active != start) {
            p->rules[i].active = start;
            changed++;
        }
    }
    if (!changed) {
        printf("no rules to %s\n", start ? "start" : "stop");
        return 0;
    }

    /* For a stop the rules are already inactive, so the interfaces to converge
       are the ones that were active a moment ago: take the list from the flags
       we saved, not from the current ones. */
    GPtrArray *ifs;
    if (start) {
        ifs = tc_profile_ifaces(p, TRUE);
    } else {
        for (int i = 0; i < p->count; i++)
            p->rules[i].active = saved[i];
        ifs = tc_profile_ifaces(p, TRUE);
        for (int i = 0; i < p->count; i++)
            p->rules[i].active = FALSE;
    }

    int rc = 0;
    for (guint k = 0; k < ifs->len; k++) {
        const char *iface = g_ptr_array_index(ifs, k);
        char terr[2048];
        if (tc_apply_interface(p, iface, terr, sizeof terr) == 0) {
            printf("%s all rules on %s\n", start ? "started" : "stopped", iface);
            continue;
        }
        if (!start) {
            rc = 1;
            for (int i = 0; i < p->count; i++)
                if (g_strcmp0(p->rules[i].iface, iface) == 0) p->rules[i].active = saved[i];
            fprintf(stderr, "error: stop on %s: %s\n", iface, terr);
            continue;
        }
        rc = 1;
        fprintf(stderr, "error: start on %s failed: %s\n", iface, terr);
        /* Roll this interface back to the state it was in and re-converge, so
           a failure here leaves the rules that were already running alone. */
        for (int i = 0; i < p->count; i++)
            if (g_strcmp0(p->rules[i].iface, iface) == 0)
                p->rules[i].active = saved[i];
        char rerr[2048];
        if (tc_apply_interface(p, iface, rerr, sizeof rerr) != 0)
            fprintf(stderr, "warning: could not restore %s: %s\n", iface, rerr);
    }
    g_ptr_array_free(ifs, TRUE);
    return rc;
}

static int cmd_start(LagProfile *p, const char *path, const char *target) {
    if (g_strcmp0(target, "all") != 0) {
        char err[512];
        int idx = parse_index(target, p, err, sizeof err);
        if (idx < 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        LagRule *r = &p->rules[idx];
        if (r->active) {
            printf("rule %d already active\n", idx);
            return 0;
        }
        r->active = TRUE;
        char terr[2048];
        if (tc_apply_interface(p, r->iface, terr, sizeof terr) != 0) {
            r->active = FALSE;
            fprintf(stderr, "error: start failed: %s\n", terr);
            return 1;
        }
        printf("started rule %d on %s\n", idx, r->iface);
        if (save_profile(p, path) != 0)
            return 1;
        return 0;
    }
    int rc = start_stop_all(p, TRUE);
    if (save_profile(p, path) != 0)
        rc = 1;
    return rc;
}

static int cmd_stop(LagProfile *p, const char *path, const char *target) {
    if (g_strcmp0(target, "all") != 0) {
        char err[512];
        int idx = parse_index(target, p, err, sizeof err);
        if (idx < 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        LagRule *r = &p->rules[idx];
        if (!r->active) {
            printf("rule %d not active\n", idx);
            return 0;
        }
        r->active = FALSE;
        char terr[2048];
        if (tc_apply_interface(p, r->iface, terr, sizeof terr) != 0) {
            r->active = TRUE;
            fprintf(stderr, "error: stop on %s: %s\n", r->iface, terr);
            return 1;
        } else
            printf("stopped rule %d on %s\n", idx, r->iface);
        if (save_profile(p, path) != 0)
            return 1;
        return 0;
    }
    int rc = start_stop_all(p, FALSE);
    if (save_profile(p, path) != 0)
        rc = 1;
    return rc;
}

static int cmd_status(LagProfile *p) {
    char ifaces[LAG_MAX_RULES][LAG_IFACE_LEN];
    int n = 0;
    for (int i = 0; i < p->count; i++) {
        const char *ifc = p->rules[i].iface;
        if (!ifc[0])
            continue;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (g_strcmp0(ifaces[k], ifc) == 0) { dup = 1; break; }
        if (!dup && n < LAG_MAX_RULES) {
            g_strlcpy(ifaces[n], ifc, LAG_IFACE_LEN);
            n++;
        }
    }
    if (n == 0) {
        printf("(no rules defined)\n");
        return 0;
    }
    for (int k = 0; k < n; k++) {
        char *txt = tc_get_state_text(ifaces[k]);
        printf("=== %s ===\n%s\n\n", ifaces[k], txt);
        g_free(txt);
    }
    return 0;
}

/* ---------------- dispatch ---------------- */

int cli_main(char **argv, LagProfile *profile, const char *profile_path) {
    if (argv[0] == NULL || g_strcmp0(argv[0], "help") == 0) {
        cli_usage(stdout);
        return argv[0] == NULL ? 1 : 0;
    }
    const char *cmd = argv[0];
    char **opts = argv + 1;

    if (g_strcmp0(cmd, "list") == 0) {
        if (opts[0]) {
            fprintf(stderr, "error: list takes no arguments\n");
            return 1;
        }
        return cmd_list(profile);
    }
    if (g_strcmp0(cmd, "ifaces") == 0) {
        if (opts[0]) {
            fprintf(stderr, "error: ifaces takes no arguments\n");
            return 1;
        }
        return cmd_ifaces();
    }
    if (g_strcmp0(cmd, "status") == 0) {
        if (opts[0]) {
            fprintf(stderr, "error: status takes no arguments\n");
            return 1;
        }
        return cmd_status(profile);
    }
    if (g_strcmp0(cmd, "add") == 0)
        return cmd_add(profile, profile_path, opts);
    if (g_strcmp0(cmd, "start") == 0 || g_strcmp0(cmd, "stop") == 0) {
        if (!opts[0]) {
            fprintf(stderr, "error: %s requires N or 'all'\n", cmd);
            return 1;
        }
        if (opts[1]) {
            fprintf(stderr, "error: %s takes one argument\n", cmd);
            return 1;
        }
        return g_strcmp0(cmd, "start") == 0
            ? cmd_start(profile, profile_path, opts[0])
            : cmd_stop(profile, profile_path, opts[0]);
    }
    if (g_strcmp0(cmd, "del") == 0 || g_strcmp0(cmd, "delete") == 0) {
        if (!opts[0] || opts[1]) {
            fprintf(stderr, "error: del requires exactly one argument (rule index)\n");
            return 1;
        }
        char err[512];
        int idx = parse_index(opts[0], profile, err, sizeof err);
        if (idx < 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        return cmd_del(profile, profile_path, idx);
    }
    if (g_strcmp0(cmd, "edit") == 0) {
        if (!opts[0]) {
            fprintf(stderr, "error: edit requires a rule index\n");
            return 1;
        }
        char err[512];
        int idx = parse_index(opts[0], profile, err, sizeof err);
        if (idx < 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        return cmd_edit(profile, profile_path, idx, opts + 1);
    }

    fprintf(stderr, "error: unknown command: %s\n\n", cmd);
    cli_usage(stderr);
    return 1;
}
