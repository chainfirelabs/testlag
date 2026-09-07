/* SPDX-License-Identifier: MIT */
/* lag — tc netem shaper. Entry point: arg parsing, profile load,
 * GUI or CLI dispatch. */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "askpass.h"
#include "cli.h"
#include "lag.h"
#include "profile.h"
#include "tc.h"
#include "ui.h"

static void usage(const char *prog) {
    fprintf(stderr,
        "TestLag — per-IP/port latency, jitter, loss & bandwidth shaper (tc/netem)\n"
        "\n"
        "Usage: %s [options]                    start the GUI\n"
        "       %s [options] <command> [args]   run a CLI command (no GUI)\n"
        "\n"
        "Global options:\n"
        "  --sudo         Force sudo for tc write commands (default when not root)\n"
        "  --no-sudo      Never use sudo (run as root, e.g. 'sudo %s')\n"
        "  --profile P    Profile file to load on start and save on exit\n"
        "                 (default: ~/.config/testlag/profile.json)\n"
        "  -h, --help     Show this help\n"
        "\n"
        "CLI commands: list, add, edit, del, start, stop, status, ifaces, help\n"
        "  See '%s help' for command details.\n"
        "\n"
        "GUI actions show a password dialog; CLI actions prompt in the terminal.\n",
        prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    /* sudo invokes this same binary as its helper, before profile/CLI parsing. */
    if (g_strcmp0(g_getenv("TESTLAG_ASKPASS"), "1") == 0)
        return askpass_run(argc > 1 ? argv[1] : "Password:");

    gboolean use_sudo = TRUE;
    char *profile_path = NULL;
    char **cli_argv = NULL;   /* command word + args; NULL = GUI mode */

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--sudo") == 0) {
            use_sudo = TRUE;
        } else if (g_strcmp0(argv[i], "--no-sudo") == 0) {
            use_sudo = FALSE;
        } else if (g_strcmp0(argv[i], "--profile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--profile requires a path\n");
                return 1;
            }
            profile_path = g_strdup(argv[++i]);
        } else if (g_strcmp0(argv[i], "--cli") == 0 ||
                   g_strcmp0(argv[i], "-c") == 0) {
            cli_argv = &argv[i + 1];
            break;
        } else if (g_strcmp0(argv[i], "-h") == 0 ||
                   g_strcmp0(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (cli_is_command(argv[i])) {
            cli_argv = &argv[i];
            break;
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    if (!profile_path)
        profile_path = profile_default_path();

    tc_set_use_sudo(use_sudo);

    LagProfile *profile = g_new0(LagProfile, 1);
    profile_clear(profile);
    if (g_file_test(profile_path, G_FILE_TEST_EXISTS)) {
        char err[512];
        if (profile_load(profile, profile_path, err, sizeof err) != 0)
            g_warning("Could not load profile %s: %s", profile_path, err);
        else if (err[0])
            g_warning("Profile %s: %s", profile_path, err);
        /* The profile says which rules were running when it was saved; the
         * kernel says which ones still are. Trusting the file would show
         * rules as active with nothing applied -- and Start is a no-op on an
         * already-active rule, so they could not be started again. */
        int stale = tc_sync_active_state(profile);
        if (stale > 0)
            g_message("%d rule(s) marked active in %s are no longer applied; "
                      "marked stopped.", stale, profile_path);
    }

    if (cli_argv) {
        int rc = cli_main(cli_argv, profile, profile_path);
        g_free(profile);
        g_free(profile_path);
        return rc;
    }

    gtk_init(&argc, &argv);
    ui_run(profile, profile_path, use_sudo);

    g_free(profile);
    g_free(profile_path);
    return 0;
}
