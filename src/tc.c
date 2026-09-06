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
#include <errno.h>
#include <signal.h>
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

/* Most output we ever keep from a child. Only the first part is ever shown in
 * an error message, and the pipe still has to be drained to EOF whatever the
 * child writes, so read everything but stop accumulating past this. */
#define TC_OUTPUT_MAX 65536

/* Read a pipe to EOF, keeping at most TC_OUTPUT_MAX bytes. Draining to the end
 * matters: a child left blocked writing to a full pipe would never exit, and
 * the waitpid() below would block with it.
 * Newly allocated, never NULL. */
static char *read_all(int fd) {
    GString *s = g_string_new("");
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            if (s->len < TC_OUTPUT_MAX)
                g_string_append_len(s, buf, MIN((gsize)n, TC_OUTPUT_MAX - s->len));
            continue;
        }
        if (n == 0 || errno != EINTR)
            break;
    }
    return g_string_free(s, FALSE);
}

/* Largest batch we will hand to tc in one write. The whole batch is written
 * before the child's output is read, so it has to fit in the pipe buffer
 * (64 KiB on Linux) or writer and reader could deadlock.
 *
 * A full profile stays inside this: the longest a rule can contribute is its
 * class + netem + filter lines with every field at its maximum
 * (LAG_IFACE_LEN-1 interface, two LAG_IP_LEN-1 addresses, two 5-digit ports,
 * a LAG_BW_LEN-1 rate), which is 640 bytes, so LAG_MAX_RULES rules cannot
 * exceed 40 KiB. The check is the backstop for a future field or rule-count
 * change: it fails the apply loudly instead of hanging on a full pipe. */
#define TC_BATCH_MAX 49152

/* Run several tc commands in a single privileged process.
 *
 * Building an interface takes seven to ten tc commands. Running them one at a
 * time meant, unprivileged, seven to ten separate sudo invocations -- and
 * every sudo is a PAM/logind session, which in bulk is enough to run a machine
 * out of file descriptors. tc's batch mode takes the same commands on stdin
 * and applies them in one process, so an apply costs one privilege transition.
 *
 * No shell is involved here either: the batch is data on the child's stdin,
 * never a command line, and every value in it comes from the validators above
 * -- none of which admit a newline, so a value cannot open a line of its own.
 * The newline check below is a second lock on that door. */
int tc_run_batch(const char *const *lines, int n, char *out, size_t outlen) {
    if (out && outlen) out[0] = '\0';
    if (!lines || n <= 0) return -1;

    GString *batch = g_string_new("");
    for (int i = 0; i < n; i++) {
        if (!lines[i] || !*lines[i] || strpbrk(lines[i], "\n\r")) {
            if (out && outlen) g_strlcpy(out, "malformed tc batch line", outlen);
            g_string_free(batch, TRUE);
            return -1;
        }
        g_string_append(batch, lines[i]);
        g_string_append_c(batch, '\n');
    }
    if (batch->len > TC_BATCH_MAX) {
        if (out && outlen) g_strlcpy(out, "tc batch too large", outlen);
        g_string_free(batch, TRUE);
        return -1;
    }

    char *prog = resolve_program("tc");
    if (!prog) {
        if (out && outlen) g_strlcpy(out, "tc: command not found", outlen);
        g_string_free(batch, TRUE);
        return -1;
    }
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    if (!tc_is_root() && g_use_sudo) {
        char *sudo = resolve_program("sudo");
        if (!sudo) {
            if (out && outlen) g_strlcpy(out, "sudo: command not found", outlen);
            g_free(prog);
            g_ptr_array_free(argv, TRUE);
            g_string_free(batch, TRUE);
            return -1;
        }
        g_ptr_array_add(argv, sudo);
    }
    g_ptr_array_add(argv, prog);
    g_ptr_array_add(argv, g_strdup("-batch"));
    g_ptr_array_add(argv, g_strdup("-"));      /* read the batch from stdin */
    g_ptr_array_add(argv, NULL);

    char **envp = child_envp();
    GPid pid = 0;
    int in_fd = -1, out_fd = -1, err_fd = -1;
    GError *gerr = NULL;
    /* sudo reads a password from /dev/tty, not stdin, so handing the child a
       pipe on stdin does not stop it prompting. */
    gboolean ok = g_spawn_async_with_pipes(NULL, (char **)argv->pdata, envp,
        G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid,
        &in_fd, &out_fd, &err_fd, &gerr);
    if (!ok) {
        if (out && outlen)
            g_snprintf(out, outlen, "%s",
                       gerr ? gerr->message : "could not run tc");
        g_clear_error(&gerr);
        g_strfreev(envp);
        g_ptr_array_free(argv, TRUE);
        g_string_free(batch, TRUE);
        return -1;
    }

    /* A child that dies before reading the batch must not take us with it. */
    void (*old_pipe)(int) = signal(SIGPIPE, SIG_IGN);
    const char *w = batch->str;
    gsize left = batch->len;
    while (left > 0) {
        ssize_t written = write(in_fd, w, left);
        if (written > 0) { w += written; left -= (gsize)written; continue; }
        if (written < 0 && errno == EINTR) continue;
        break;                              /* child gone: its status tells us */
    }
    close(in_fd);
    signal(SIGPIPE, old_pipe);

    char *sout = read_all(out_fd);
    char *serr = read_all(err_fd);
    close(out_fd);
    close(err_fd);
    if (out && outlen) {
        g_strlcpy(out, sout, outlen);
        if (*serr) g_strlcat(out, serr, outlen);
    }
    g_free(sout);
    g_free(serr);

    int wstatus = 0, status;
    pid_t got;
    do { got = waitpid((pid_t)pid, &wstatus, 0); } while (got < 0 && errno == EINTR);
    status = (got == (pid_t)pid && WIFEXITED(wstatus)) ? WEXITSTATUS(wstatus) : -1;
    g_spawn_close_pid(pid);

    g_strfreev(envp);
    g_ptr_array_free(argv, TRUE);
    g_string_free(batch, TRUE);
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

/* An address is IPv6 if it carries a colon; a rule is IPv6 if either of its
 * addresses is (tc_valid_rule rejects a rule that mixes the two families). */
static gboolean ip_is_v6(const char *s) {
    return s[0] != '\0' && strchr(s, ':') != NULL;
}

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
    if (r->src_ip[0] && !tc_valid_ip(r->src_ip))
        return rule_err(err, errlen, "invalid source IP address: %s", r->src_ip);
    if (r->dst_ip[0] && !tc_valid_ip(r->dst_ip))
        return rule_err(err, errlen, "invalid destination IP address: %s", r->dst_ip);
    if (r->src_port[0] && !tc_valid_port(r->src_port))
        return rule_err(err, errlen, "invalid source port: %s", r->src_port);
    if (r->dst_port[0] && !tc_valid_port(r->dst_port))
        return rule_err(err, errlen, "invalid destination port: %s", r->dst_port);
    /* One u32 filter matches one protocol: a rule cannot straddle v4 and v6. */
    if (r->src_ip[0] && r->dst_ip[0] &&
        ip_is_v6(r->src_ip) != ip_is_v6(r->dst_ip))
        return rule_err(err, errlen,
            "source and destination must be the same IP version (%s / %s)",
            r->src_ip, r->dst_ip);
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

/* Read the interface's root qdisc once.
 * Returns 0 if TestLag may manage the interface, with *needs_teardown set when
 * a real root qdisc is in place and has to be deleted before ours can be
 * added; -1 with err filled if a foreign root qdisc is in the way.
 * `noqueue` is the kernel's placeholder for "no root qdisc", not something to
 * delete: htb can be added straight over it. */
static int check_interface_state(const char *iface, gboolean *needs_teardown,
                                 char *err, size_t errlen) {
    if (needs_teardown) *needs_teardown = FALSE;
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
    int rc = 0;                 /* no root line at all => clean */
    for (int i = 0; lines[i]; i++) {
        char *l = lines[i];
        if (!strstr(l, "root"))
            continue; /* only the root qdisc line contains "root" */
        if (strstr(l, "noqueue"))
            break;              /* kernel placeholder: add straight over it */
        if (strstr(l, "fq_codel") || strstr(l, "htb")) {
            /* kernel default, or our own root from a previous run: ours to
               replace, but the existing qdisc has to go first */
            if (needs_teardown) *needs_teardown = TRUE;
            break;
        }
        g_snprintf(err, errlen,
            "Interface %s already has a root qdisc:\n  %s\n"
            "TestLag manages its own root qdisc and will not touch an existing one.\n"
            "Remove it first if you want TestLag to take over:\n  tc qdisc del dev %s root",
            iface, g_strstrip(l), iface);
        rc = -1;
        break;
    }
    g_strfreev(lines);
    return rc;
}

int tc_check_interface(const char *iface, char *err, size_t errlen) {
    return check_interface_state(iface, NULL, err, errlen);
}

/* TRUE if iface currently carries a root qdisc we built (htb). Any other root
 * -- the kernel default, or a foreign qdisc -- means our rules are not applied
 * to it, whatever the profile says. */
static gboolean tc_iface_is_shaped(const char *iface) {
    if (!tc_valid_iface(iface))
        return FALSE;
    char cmd[512], out[8192];
    g_snprintf(cmd, sizeof cmd, "tc qdisc show dev %s", iface);
    if (tc_run(cmd, FALSE, out, sizeof out) != 0)
        return FALSE;   /* interface gone, or tc unreadable: not shaped by us */
    gboolean ours = FALSE;
    char **lines = g_strsplit(out, "\n", -1);
    for (int i = 0; lines[i] && !ours; i++)
        if (strstr(lines[i], "root") && strstr(lines[i], "htb"))
            ours = TRUE;
    g_strfreev(lines);
    return ours;
}

GPtrArray *tc_profile_ifaces(const LagProfile *p, gboolean active_only) {
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    if (!p)
        return arr;
    for (int i = 0; i < p->count; i++) {
        const LagRule *r = &p->rules[i];
        if (!r->iface[0] || (active_only && !r->active))
            continue;
        gboolean dup = FALSE;
        for (guint k = 0; k < arr->len && !dup; k++)
            if (g_strcmp0((const char *)g_ptr_array_index(arr, k), r->iface) == 0)
                dup = TRUE;
        if (!dup)
            g_ptr_array_add(arr, g_strdup(r->iface));
    }
    return arr;
}

int tc_sync_active_state(LagProfile *p) {
    if (!p)
        return 0;
    GPtrArray *ifs = tc_profile_ifaces(p, TRUE);
    int cleared = 0;
    for (guint k = 0; k < ifs->len; k++) {
        const char *iface = g_ptr_array_index(ifs, k);
        if (tc_iface_is_shaped(iface))
            continue;
        for (int i = 0; i < p->count; i++) {
            if (!p->rules[i].active || g_strcmp0(p->rules[i].iface, iface) != 0)
                continue;
            p->rules[i].active = FALSE;
            cleared++;
        }
    }
    g_ptr_array_free(ifs, TRUE);
    return cleared;
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

static gboolean rule_is_v6(const LagRule *r) {
    return ip_is_v6(r->src_ip) || ip_is_v6(r->dst_ip);
}

char *tc_build_filter(const LagRule *r, const char *iface, int classid) {
    if (lag_rule_is_all(r))
        return NULL;
    gboolean is_v6 = rule_is_v6(r);
    GString *s = g_string_new("filter add dev ");
    g_string_append(s, iface);
    g_string_append(s, " parent 1: protocol ");
    g_string_append(s, is_v6 ? "ipv6" : "ip");
    g_string_append(s, " u32");
    /* Source is this machine and destination is the peer: netem shapes the
     * packets leaving the interface, so a reply from a local service matches
     * on source and a request this machine makes matches on destination. */
    if (is_v6) {
        /* u32 port matching for IPv6 is not supported; addresses only */
        if (r->src_ip[0])
            g_string_append_printf(s, " match ip6 src %s", r->src_ip);
        if (r->dst_ip[0])
            g_string_append_printf(s, " match ip6 dst %s", r->dst_ip);
    } else {
        if (r->src_ip[0])
            g_string_append_printf(s, " match ip src %s/32", r->src_ip);
        if (r->dst_ip[0])
            g_string_append_printf(s, " match ip dst %s/32", r->dst_ip);
        if (r->src_port[0])
            g_string_append_printf(s, " match ip sport %s 0xffff", r->src_port);
        if (r->dst_port[0])
            g_string_append_printf(s, " match ip dport %s 0xffff", r->dst_port);
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
    gboolean needs_teardown = FALSE;
    if (check_interface_state(iface, &needs_teardown, err, errlen) != 0)
        return -1;

    int active_count = 0;
    for (int i = 0; i < p->count; i++)
        if (p->rules[i].active && g_strcmp0(p->rules[i].iface, iface) == 0)
            active_count++;

    char cmd[512], out[8192];

    if (active_count == 0) {
        /* nothing to shape: restore clean state */
        if (needs_teardown) {
            g_snprintf(cmd, sizeof cmd, "tc qdisc del dev %s root", iface);
            tc_run(cmd, TRUE, out, sizeof out);
        }
        return 0;
    }

    /* Build the whole tree as one batch before touching the interface: a rule
     * set we cannot express leaves the running configuration alone. */
    GPtrArray *batch = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(batch, g_strdup_printf(
        "qdisc add dev %s root handle 1: htb default 1", iface));
    g_ptr_array_add(batch, g_strdup_printf(
        "class add dev %s parent 1: classid 1:1 htb rate %s ceil %s",
        iface, LAG_CLASS_RATE, LAG_CLASS_RATE));

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
                g_ptr_array_free(batch, TRUE);
                return -1;
            }
            all_used = TRUE;
            cls = 1;            /* the htb default class, already created */
        } else {
            cls = next_class++;
            g_ptr_array_add(batch, g_strdup_printf(
                "class add dev %s parent 1: classid 1:%d htb rate %s ceil %s",
                iface, cls, LAG_CLASS_RATE, LAG_CLASS_RATE));
        }

        char *netem = tc_build_netem(r);
        g_ptr_array_add(batch, g_strdup_printf(
            "qdisc add dev %s parent 1:%d %s", iface, cls, netem));
        g_free(netem);

        char *filt = tc_build_filter(r, iface, cls);
        if (filt)
            g_ptr_array_add(batch, filt);   /* the array owns it now */
    }
    if (!all_used) {
        /* no all-traffic rule active: give the htb default class (1:1) a
           pass-through queue, otherwise unmatched traffic has no leaf qdisc
           and is dropped by the tree. */
        g_ptr_array_add(batch, g_strdup_printf(
            "qdisc add dev %s parent 1:1 pfifo limit 1000", iface));
    }

    /* Tear down whatever we had; htb will not go on top of an existing root. */
    if (needs_teardown) {
        g_snprintf(cmd, sizeof cmd, "tc qdisc del dev %s root", iface);
        tc_run(cmd, TRUE, out, sizeof out);
    }

    int rc = tc_run_batch((const char *const *)batch->pdata, (int)batch->len,
                          out, sizeof out);
    g_ptr_array_free(batch, TRUE);
    if (rc != 0) {
        g_snprintf(err, errlen, "Failed to apply rules on %s:\n%s", iface, out);
        return -1;
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
