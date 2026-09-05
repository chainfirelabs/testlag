/* tc backend: builds and runs iproute2 `tc` commands for netem shaping.
 *
 * Per-interface layout (rebuilt atomically on every start/stop):
 *
 *   qdisc htb 1: root (default 1)
 *     class 1:1  <- default class; all-traffic rule (netem) or pass-through (pfifo)
 *     class 1:2  <- specific rule #1 (netem + u32 filter, flowid 1:2)
 *     class 1:3  <- specific rule #2 (netem + u32 filter, flowid 1:3)
 *     ...
 *
 * htb is used (not prio) so an interface can carry any number of rules.
 * Class rates are set very high so htb never limits bandwidth; the netem
 * "rate" option is the actual limiter.
 *
 * Commands are never handed to a shell: tc_run() splits the command string
 * into an argv vector and execs the binary directly (see tc_run).
 */
#include "tc.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static gboolean g_use_sudo = TRUE;

gboolean tc_is_root(void) { return geteuid() == 0; }

void     tc_set_use_sudo(gboolean use) { g_use_sudo = use; }
gboolean tc_get_use_sudo(void)         { return g_use_sudo; }

/* ---------------- command execution ---------------- */

/* Directories searched for helper binaries, in order. $PATH is deliberately
 * ignored: this process may be running as root, and an inherited PATH would
 * then decide which binary root executes. */
static const char *const PROG_DIRS[] = {
    "/sbin", "/usr/sbin", "/bin", "/usr/bin",
    "/usr/local/sbin", "/usr/local/bin", NULL
};

/* Absolute path of a helper binary, or NULL if it is not installed.
 * A name containing '/' is always refused: the caller may only name a
 * program, never a path. */
static char *resolve_program(const char *name) {
    if (!name || !*name || strchr(name, '/'))
        return NULL;
    for (int i = 0; PROG_DIRS[i]; i++) {
        char *p = g_build_filename(PROG_DIRS[i], name, NULL);
        if (g_file_test(p, G_FILE_TEST_IS_EXECUTABLE))
            return p;
        g_free(p);
    }
    return NULL;
}

/* Minimal environment for the child: a fixed PATH plus the few variables
 * sudo needs to prompt on the terminal. Caller frees with g_strfreev(). */
static char **child_envp(void) {
    static const char *const keep[] = {
        "HOME", "USER", "LOGNAME", "TERM", "LANG", "LC_ALL", "LC_MESSAGES",
        "TZ", "SUDO_ASKPASS", "DISPLAY", "XAUTHORITY", NULL
    };
    GPtrArray *e = g_ptr_array_new();
    g_ptr_array_add(e, g_strdup("PATH=/usr/sbin:/usr/bin:/sbin:/bin"));
    for (int i = 0; keep[i]; i++) {
        const char *v = g_getenv(keep[i]);
        if (v)
            g_ptr_array_add(e, g_strdup_printf("%s=%s", keep[i], v));
    }
    g_ptr_array_add(e, NULL);
    return (char **)g_ptr_array_free(e, FALSE);
}

/* Run a command with no shell involved: the command string is split on
 * whitespace into an argv vector and the binary is exec'd directly (through
 * sudo when elevation is needed). Every value interpolated into a command
 * comes from the validators below, none of which admit whitespace, so the
 * split is exact -- and a value that somehow slipped past them still cannot
 * become a second command, a redirect, or a substitution.
 *
 * Combined stdout+stderr is captured into out (if non-NULL, truncated to
 * outlen). Returns the exit status (0 = success) or -1 if the command could
 * not be launched. */
int tc_run(const char *cmd, gboolean needs_priv, char *out, size_t outlen) {
    if (out && outlen) out[0] = '\0';
    if (!cmd || !*cmd) return -1;

    char **tok = g_strsplit_set(cmd, " \t", -1);
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);

    int i = 0;
    while (tok[i] && !tok[i][0]) i++;               /* first non-empty token */
    char *prog = tok[i] ? resolve_program(tok[i]) : NULL;
    if (!prog) {
        if (out && outlen)
            g_snprintf(out, outlen, "%s: command not found",
                       tok[i] ? tok[i] : "(empty command)");
        g_strfreev(tok);
        g_ptr_array_free(argv, TRUE);
        return -1;
    }
    if (needs_priv && !tc_is_root() && g_use_sudo) {
        char *sudo = resolve_program("sudo");
        if (!sudo) {
            if (out && outlen) g_strlcpy(out, "sudo: command not found", outlen);
            g_free(prog);
            g_strfreev(tok);
            g_ptr_array_free(argv, TRUE);
            return -1;
        }
        g_ptr_array_add(argv, sudo);
    }
    g_ptr_array_add(argv, prog);
    for (i++; tok[i]; i++)
        if (tok[i][0])
            g_ptr_array_add(argv, g_strdup(tok[i]));
    g_ptr_array_add(argv, NULL);
    g_strfreev(tok);

    char **envp = child_envp();
    char *sout = NULL, *serr = NULL;
    gint wstatus = 0;
    GError *gerr = NULL;
    /* stdin is inherited so sudo can still read a password from the tty. */
    gboolean ok = g_spawn_sync(NULL, (char **)argv->pdata, envp,
                               G_SPAWN_CHILD_INHERITS_STDIN, NULL, NULL,
                               &sout, &serr, &wstatus, &gerr);
    int status;
    if (!ok) {
        if (out && outlen)
            g_snprintf(out, outlen, "%s", gerr ? gerr->message : "could not run command");
        status = -1;
    } else {
        if (out && outlen) {
            g_strlcpy(out, sout ? sout : "", outlen);
            if (serr && *serr) g_strlcat(out, serr, outlen);
        }
        status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
    }
    g_clear_error(&gerr);
    g_free(sout);
    g_free(serr);
    g_strfreev(envp);
    g_ptr_array_free(argv, TRUE);
    return status;
}

gboolean tc_sudo_present(void) {
    char *p = resolve_program("sudo");
    gboolean found = (p != NULL);
    g_free(p);
    return found;
}

/* ---------------- input validation ----------------
 *
 * Command strings are exec'd without a shell, so these are not the last line
 * of defence any more; they exist to keep malformed values (from a profile
 * file, the CLI, or the GUI) from reaching tc at all. Every rule that is
 * loaded or applied goes through tc_valid_rule(). */

gboolean tc_valid_iface(const char *s) {
    if (!s || !*s) return FALSE;
    if (strlen(s) >= LAG_IFACE_LEN) return FALSE;
    if (s[0] == '-') return FALSE;          /* never let a name look like an option */
    if (g_strcmp0(s, ".") == 0 || g_strcmp0(s, "..") == 0) return FALSE;
    for (const char *c = s; *c; c++)
        if (!g_ascii_isalnum(*c) && *c != '.' && *c != '_' && *c != '-')
            return FALSE;
    return TRUE;
}

gboolean tc_valid_ip(const char *s) {
    if (!s || !*s) return FALSE;
    if (strlen(s) >= LAG_IP_LEN) return FALSE;
    unsigned char buf[sizeof(struct in6_addr)];
    int family = strchr(s, ':') ? AF_INET6 : AF_INET;
    return inet_pton(family, s, buf) == 1;
}

gboolean tc_valid_port(const char *s) {
    if (!s || !*s || strlen(s) > 5) return FALSE;
    for (const char *c = s; *c; c++)
        if (!g_ascii_isdigit(*c)) return FALSE;
    long v = strtol(s, NULL, 10);
    return v >= 1 && v <= 65535;
}

/* netem rate: a number with an optional unit suffix, e.g. 100kbit, 1.5mbit. */
gboolean tc_valid_rate(const char *s) {
    static const char *const units[] = {
        "", "bit", "bps", "kbit", "kbps", "mbit", "mbps", "gbit", "gbps",
        "tbit", "tbps", "kibit", "kibps", "mibit", "mibps", "gibit", "gibps",
        "tibit", "tibps", NULL
    };
    if (!s || !*s || strlen(s) >= LAG_BW_LEN) return FALSE;
    const char *c = s;
    if (!g_ascii_isdigit(*c)) return FALSE;
    while (g_ascii_isdigit(*c)) c++;
    if (*c == '.') {
        c++;
        if (!g_ascii_isdigit(*c)) return FALSE;
        while (g_ascii_isdigit(*c)) c++;
    }
    for (int i = 0; units[i]; i++)
        if (g_ascii_strcasecmp(c, units[i]) == 0)
            return TRUE;
    return FALSE;
}

gboolean tc_valid_ms(double v) {
    return isfinite(v) && v >= 0.0 && v <= (double)LAG_MAX_MS;
}

gboolean tc_valid_pct(double v) {
    return isfinite(v) && v >= 0.0 && v <= 100.0;
}

static gboolean rule_err(char *err, size_t errlen, const char *fmt, ...) {
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return FALSE;
}

gboolean tc_valid_rule(const LagRule *r, char *err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    if (!r)
        return rule_err(err, errlen, "no rule");
    if (!tc_valid_iface(r->iface))
        return rule_err(err, errlen, "invalid interface name: %s", r->iface);
    if (r->ip[0] && !tc_valid_ip(r->ip))
        return rule_err(err, errlen, "invalid IP address: %s", r->ip);
    if (r->port[0] && !tc_valid_port(r->port))
        return rule_err(err, errlen, "invalid port: %s", r->port);
    if (r->bandwidth[0] && !tc_valid_rate(r->bandwidth))
        return rule_err(err, errlen, "invalid bandwidth: %s", r->bandwidth);
    if (!tc_valid_ms(r->latency_ms))
        return rule_err(err, errlen, "latency out of range (0-%d ms)", LAG_MAX_MS);
    if (!tc_valid_ms(r->jitter_ms))
        return rule_err(err, errlen, "jitter out of range (0-%d ms)", LAG_MAX_MS);
    if (!tc_valid_pct(r->drop_pct))
        return rule_err(err, errlen, "drop out of range (0-100 %%)");
    return TRUE;
}

/* ---------------- interface discovery ---------------- */

GPtrArray *tc_list_interfaces(void) {
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    char out[8192];
    if (tc_run("ip -o link show", FALSE, out, sizeof out) != 0)
        return arr;
    /* lines look like: "2: eth0: <BROADCAST,...> mtu 1500 ..." */
    char **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = lines[i];
        if (!g_ascii_isdigit(line[0]))
            continue; /* skip "link/..." continuation lines */
        char *c1 = strchr(line, ':');
        if (!c1) continue;
        char *name = c1 + 1;
        char *c2 = strchr(name, ':');
        if (c2) *c2 = '\0';
        name = g_strstrip(name);
        if (name[0] && tc_valid_iface(name))
            g_ptr_array_add(arr, g_strdup(name));
    }
    g_strfreev(lines);
    return arr;
}

/* ---------------- state inspection ---------------- */

int tc_check_interface(const char *iface, char *err, size_t errlen) {
    if (!tc_valid_iface(iface)) {
        g_snprintf(err, errlen, "Invalid interface name: %s", iface ? iface : "(none)");
        return -1;
    }
    char cmd[512], out[8192];
    g_snprintf(cmd, sizeof cmd, "tc qdisc show dev %s", iface);
    if (tc_run(cmd, FALSE, out, sizeof out) != 0) {
        g_snprintf(err, errlen,
            "Cannot read qdisc state for %s (is the interface up?):\n%s", iface, out);
        return -1;
    }
    char **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *l = lines[i];
        if (!strstr(l, "root"))
            continue; /* only the root qdisc line contains "root" */
        if (strstr(l, "noqueue") || strstr(l, "fq_codel")) {
            /* kernel default root: safe to replace */
            g_strfreev(lines);
            return 0;
        }
        if (strstr(l, "htb")) {
            /* our own root from a previous run: safe to rebuild */
            g_strfreev(lines);
            return 0;
        }
        g_snprintf(err, errlen,
            "Interface %s already has a root qdisc:\n  %s\n"
            "TestLag manages its own root qdisc and will not touch an existing one.\n"
            "Remove it first if you want TestLag to take over:\n  tc qdisc del dev %s root",
            iface, g_strstrip(l), iface);
        g_strfreev(lines);
        return -1;
    }
    g_strfreev(lines);
    return 0; /* no root line at all => clean */
}

char *tc_get_state_text(const char *iface) {
    if (!tc_valid_iface(iface))
        return g_strdup_printf("(invalid interface name: %s)\n",
                               iface ? iface : "(none)");
    GString *s = g_string_new("");
    char cmd[512], out[16384];
    g_snprintf(cmd, sizeof cmd, "tc qdisc show dev %s", iface);
    if (tc_run(cmd, FALSE, out, sizeof out) == 0)
        g_string_append(s, out);
    g_snprintf(cmd, sizeof cmd, "tc filter show dev %s", iface);
    char out2[8192];
    if (tc_run(cmd, FALSE, out2, sizeof out2) == 0) {
        if (s->len) g_string_append_c(s, '\n');
        g_string_append(s, out2);
    }
    return g_string_free(s, FALSE);
}

/* ---------------- command builders ---------------- */

/* Append a non-negative number without trailing zeros or sci notation.
 * Non-finite values cannot reach a command line. */
static void append_num(GString *s, double v) {
    char buf[64];
    if (!isfinite(v) || v < 0)
        v = 0;
    if (v < 1e15 && v == (double)(long long)v)
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

char *tc_build_netem(const LagRule *r) {
    GString *s = g_string_new("netem");
    if (r->latency_ms > 0 || r->jitter_ms > 0) {
        g_string_append(s, " delay ");
        append_num(s, r->latency_ms);
        g_string_append(s, "ms");
        if (r->jitter_ms > 0) {
            g_string_append_c(s, ' ');
            append_num(s, r->jitter_ms);
            g_string_append(s, "ms");
        }
    }
    if (r->drop_pct > 0) {
        g_string_append(s, " loss ");
        append_num(s, r->drop_pct);
        g_string_append_c(s, '%');
    }
    if (r->bandwidth[0])
        g_string_append_printf(s, " rate %s", r->bandwidth);
    return g_string_free(s, FALSE);
}

char *tc_build_filter(const LagRule *r, const char *iface, int classid) {
    if (lag_rule_is_all(r))
        return NULL;
    gboolean is_v6 = (r->ip[0] != '\0' && strchr(r->ip, ':') != NULL);
    GString *s = g_string_new("tc filter add dev ");
    g_string_append(s, iface);
    g_string_append(s, " parent 1: protocol ");
    g_string_append(s, is_v6 ? "ipv6" : "ip");
    g_string_append(s, " u32 ");
    if (is_v6) {
        /* u32 port matching for IPv6 is not supported; dst only */
        g_string_append_printf(s, "match ip6 dst %s", r->ip);
    } else {
        if (r->ip[0])
            g_string_append_printf(s, "match ip dst %s/32", r->ip);
        if (r->port[0])
            g_string_append_printf(s, " match ip dport %s 0xffff", r->port);
    }
    g_string_append_printf(s, " flowid 1:%d", classid);
    return g_string_free(s, FALSE);
}

/* ---------------- apply / clear ---------------- */

int tc_apply_interface(LagProfile *p, const char *iface, char *err, size_t errlen) {
    if (!tc_valid_iface(iface)) {
        g_snprintf(err, errlen, "Invalid interface name: %s", iface ? iface : "(none)");
        return -1;
    }
    /* Last gate before anything is exec'd: every rule we are about to build a
     * command from must be well-formed, whatever its origin. */
    for (int i = 0; i < p->count; i++) {
        if (!p->rules[i].active || g_strcmp0(p->rules[i].iface, iface) != 0)
            continue;
        char rerr[256];
        if (!tc_valid_rule(&p->rules[i], rerr, sizeof rerr)) {
            g_snprintf(err, errlen, "Rule %d is not valid: %s", i, rerr);
            return -1;
        }
    }
    if (tc_check_interface(iface, err, errlen) != 0)
        return -1;

    int active_count = 0;
    for (int i = 0; i < p->count; i++)
        if (p->rules[i].active && g_strcmp0(p->rules[i].iface, iface) == 0)
            active_count++;

    char cmd[2048], out[8192];

    if (active_count == 0) {
        /* nothing to shape: restore clean state (ignore "no qdisc" errors) */
        g_snprintf(cmd, sizeof cmd, "tc qdisc del dev %s root", iface);
        tc_run(cmd, TRUE, out, sizeof out);
        return 0;
    }

    /* teardown whatever we had, then rebuild from scratch */
    g_snprintf(cmd, sizeof cmd, "tc qdisc del dev %s root", iface);
    tc_run(cmd, TRUE, out, sizeof out);

    g_snprintf(cmd, sizeof cmd,
               "tc qdisc add dev %s root handle 1: htb default 1", iface);
    if (tc_run(cmd, TRUE, out, sizeof out) != 0) {
        g_snprintf(err, errlen, "Failed to add htb root on %s:\n%s", iface, out);
        return -1;
    }

    g_snprintf(cmd, sizeof cmd,
               "tc class add dev %s parent 1: classid 1:1 htb rate %s ceil %s",
               iface, LAG_CLASS_RATE, LAG_CLASS_RATE);
    if (tc_run(cmd, TRUE, out, sizeof out) != 0) {
        g_snprintf(err, errlen, "Failed to add class 1:1 on %s:\n%s", iface, out);
        return -1;
    }

    gboolean all_used = FALSE;
    int next_class = 2;
    for (int i = 0; i < p->count; i++) {
        LagRule *r = &p->rules[i];
        if (!r->active || g_strcmp0(r->iface, iface) != 0)
            continue;

        int cls;
        if (lag_rule_is_all(r)) {
            if (all_used) {
                g_snprintf(err, errlen,
                    "Interface %s has more than one all-traffic rule active; "
                    "only one is allowed.", iface);
                return -1;
            }
            all_used = TRUE;
            cls = 1;
        } else {
            cls = next_class++;
        }

        if (!lag_rule_is_all(r)) {
            g_snprintf(cmd, sizeof cmd,
                       "tc class add dev %s parent 1: classid 1:%d htb rate %s ceil %s",
                       iface, cls, LAG_CLASS_RATE, LAG_CLASS_RATE);
            if (tc_run(cmd, TRUE, out, sizeof out) != 0) {
                g_snprintf(err, errlen,
                    "Failed to add class 1:%d on %s:\n%s", cls, iface, out);
                return -1;
            }
        }
        char *netem = tc_build_netem(r);
        g_snprintf(cmd, sizeof cmd,
                   "tc qdisc add dev %s parent 1:%d %s", iface, cls, netem);
        g_free(netem);
        if (tc_run(cmd, TRUE, out, sizeof out) != 0) {
            g_snprintf(err, errlen,
                "Failed to add netem to %s (class 1:%d):\n%s", iface, cls, out);
            return -1;
        }

        char *filt = tc_build_filter(r, iface, cls);
        if (filt) {
            int rc = tc_run(filt, TRUE, out, sizeof out);
            g_free(filt);
            if (rc != 0) {
                g_snprintf(err, errlen, "Failed to add filter on %s:\n%s", iface, out);
                return -1;
            }
        }
    }
    if (!all_used) {
        /* no all-traffic rule active: give the htb default class (1:1) a
           pass-through queue, otherwise unmatched traffic has no leaf qdisc
           and is dropped by the tree. */
        g_snprintf(cmd, sizeof cmd,
                   "tc qdisc add dev %s parent 1:1 pfifo limit 1000", iface);
        if (tc_run(cmd, TRUE, out, sizeof out) != 0) {
            g_snprintf(err, errlen,
                "Failed to add pass-through qdisc on %s:\n%s", iface, out);
            return -1;
        }
    }
    return 0;
}

int tc_clear_interface(const char *iface, char *err, size_t errlen) {
    if (!tc_valid_iface(iface)) {
        g_snprintf(err, errlen, "Invalid interface name: %s", iface ? iface : "(none)");
        return -1;
    }
    char cmd[512], out[8192];
    g_snprintf(cmd, sizeof cmd, "tc qdisc del dev %s root", iface);
    int rc = tc_run(cmd, TRUE, out, sizeof out);
    if (rc != 0) {
        /* "no qdisc to delete" is not an error for us */
        if (strstr(out, "No such") || strstr(out, "no qdisc") || strstr(out, "Cannot find"))
            return 0;
        g_snprintf(err, errlen, "tc qdisc del failed on %s:\n%s", iface, out);
        return -1;
    }
    return 0;
}
