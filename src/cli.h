/* SPDX-License-Identifier: MIT */
#ifndef LAG_CLI_H
#define LAG_CLI_H

#include <glib.h>
#include "lag.h"

/* True if s is a known CLI command word (main uses this to detect bare
 * subcommands like `lag list`). */
gboolean cli_is_command(const char *s);

/* Run one CLI command and return the process exit code (0 = success).
 * argv[0] is the command word, argv[1..] its arguments.
 * Mutating commands (add/edit/del/start/stop) save the profile to
 * profile_path after success. */
int cli_main(char **argv, LagProfile *profile, const char *profile_path);

#endif /* LAG_CLI_H */
