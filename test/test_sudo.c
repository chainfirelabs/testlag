/* SPDX-License-Identifier: MIT */
/* Intercept process launches: no sudo authentication or network writes. */
#include <glib.h>
#include <unistd.h>
#include "tc.h"

static uid_t test_uid = 1000;
static char **last_argv, **last_env;
static int batch_calls;
static int delete_calls;
static gboolean fail_delete;
static const char *qdisc_state = "";

uid_t __wrap_geteuid(void) { return test_uid; }

static void capture(char **argv, char **env) {
    g_strfreev(last_argv);
    g_strfreev(last_env);
    last_argv = g_strdupv(argv);
    last_env = g_strdupv(env);
}

gboolean __wrap_g_spawn_sync(const gchar *cwd, gchar **argv, gchar **env,
    GSpawnFlags flags, GSpawnChildSetupFunc setup, gpointer data,
    gchar **out, gchar **err, gint *status, GError **error) {
    (void)cwd; (void)flags; (void)setup; (void)data; (void)error;
    capture(argv, env);
    gboolean deleting = g_strv_contains((const gchar *const *)argv, "del");
    if (deleting) delete_calls++;
    *out = g_strdup(deleting ? "" : qdisc_state);
    *err = g_strdup(fail_delete && deleting ? "sudo: no password was provided" : "");
    *status = fail_delete && deleting ? 1 << 8 : 0;
    return TRUE;
}

gboolean __wrap_g_spawn_async_with_pipes(const gchar *cwd, gchar **argv,
    gchar **env, GSpawnFlags flags, GSpawnChildSetupFunc setup, gpointer data,
    GPid *pid, gint *in, gint *out, gint *err, GError **error) {
    (void)cwd; (void)flags; (void)setup; (void)data;
    (void)pid; (void)in; (void)out; (void)err;
    capture(argv, env);
    batch_calls++;
    g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "test launch stopped");
    return FALSE;
}

static void check_launch(gboolean sudo, gboolean askpass) {
    g_assert_true(g_str_has_suffix(last_argv[0], sudo ? "/sudo" : "/tc"));
    g_assert_cmpint(g_strv_contains((const gchar *const *)last_argv, "-A"), ==, askpass);
    g_assert_cmpstr(g_environ_getenv(last_env, "TESTLAG_ASKPASS"), ==, askpass ? "1" : NULL);
    g_assert_cmpstr(g_environ_getenv(last_env, "SUDO_ASKPASS"), ==,
        askpass ? "/tmp/test lag" : "/existing/helper");
}

int main(void) {
    g_setenv("SUDO_ASKPASS", "/existing/helper", TRUE);
    g_setenv("WAYLAND_DISPLAY", "wayland-test", TRUE);
    char out[1024];
    const char *lines[] = {"qdisc show dev lo"};
    for (int mode = 0; mode < 4; mode++) {
        tc_set_askpass(mode == 0 ? NULL : "/tmp/test lag");
        test_uid = mode == 2 ? 0 : 1000;
        tc_set_use_sudo(mode != 3);
        gboolean sudo = mode < 2, askpass = mode == 1;
        g_assert_cmpint(tc_run("tc qdisc show dev lo", TRUE, out, sizeof out), ==, 0);
        check_launch(sudo, askpass);
        g_assert_cmpstr(g_environ_getenv(last_env, "WAYLAND_DISPLAY"), ==, "wayland-test");
        g_assert_cmpint(tc_run_batch(lines, 1, out, sizeof out), ==, -1);
        check_launch(sudo, askpass);
        g_assert_cmpint(tc_run("tc qdisc show dev lo", FALSE, out, sizeof out), ==, 0);
        check_launch(FALSE, FALSE);
    }
    tc_set_use_sudo(TRUE);
    fail_delete = TRUE;
    qdisc_state = "qdisc htb 1: root\n";
    LagProfile profile = {0};
    profile.count = 1;
    g_strlcpy(profile.rules[0].iface, "lo", sizeof profile.rules[0].iface);
    profile.rules[0].latency_ms = 10;
    int before = batch_calls;
    for (int active = 0; active < 2; active++) {
        profile.rules[0].active = active;
        g_assert_cmpint(tc_apply_interface(&profile, "lo", out, sizeof out), ==, -1);
        g_assert_nonnull(strstr(out, "no password was provided"));
        g_assert_cmpint(batch_calls, ==, before);
    }

    /* A kernel default must not be deleted, even when stopping an inactive
     * profile. Keep deletion failing to catch any accidental privileged call. */
    const char *defaults[] = {
        "qdisc fq_codel 0: root refcnt 2 limit 10240p flows 1024\n",
        "qdisc fq_codel 0:0 root refcnt 2\n",
        "qdisc noqueue 0: root refcnt 2\n",
    };
    for (guint i = 0; i < G_N_ELEMENTS(defaults); i++) {
        qdisc_state = defaults[i];
        int deletes_before = delete_calls;
        before = batch_calls;
        profile.rules[0].active = FALSE;
        g_assert_cmpint(tc_apply_interface(&profile, "lo", out, sizeof out), ==, 0);
        g_assert_cmpint(delete_calls, ==, deletes_before);
        g_assert_cmpint(batch_calls, ==, before);
        profile.rules[0].active = TRUE;
        /* The intercepted batch launch fails deliberately; reaching it proves
         * the undeletable default does not block starting the rules. */
        g_assert_cmpint(tc_apply_interface(&profile, "lo", out, sizeof out), ==, -1);
        g_assert_nonnull(strstr(out, "test launch stopped"));
        g_assert_cmpint(delete_calls, ==, deletes_before);
        g_assert_cmpint(batch_calls, ==, before + 1);
    }
    qdisc_state = "qdisc fq_codel 8001: root refcnt 2\n";
    before = batch_calls;
    g_assert_cmpint(tc_apply_interface(&profile, "lo", out, sizeof out), ==, -1);
    g_assert_nonnull(strstr(out, "no password was provided"));
    g_assert_cmpint(batch_calls, ==, before);

    /* A zero handle alone does not give permission to replace foreign types. */
    qdisc_state = "qdisc cake 0: root refcnt 2\n";
    int deletes_before = delete_calls;
    g_assert_cmpint(tc_apply_interface(&profile, "lo", out, sizeof out), ==, -1);
    g_assert_nonnull(strstr(out, "already has a root qdisc"));
    g_assert_cmpint(delete_calls, ==, deletes_before);
    g_assert_cmpint(batch_calls, ==, before);
    tc_set_askpass(NULL);
    g_strfreev(last_argv);
    g_strfreev(last_env);
    g_print("Sudo: routing, cancellation, zero-handle defaults and foreign qdiscs passed.\n");
    return 0;
}
